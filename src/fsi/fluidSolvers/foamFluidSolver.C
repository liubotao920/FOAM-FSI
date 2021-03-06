
/*
 * Author
 *   David Blom, TU Delft. All rights reserved.
 */

#include "ElRBFMeshMotionSolver.H"
#include "foamFluidSolver.H"

foamFluidSolver::foamFluidSolver(
    const std::string & name,
    shared_ptr<argList> args,
    shared_ptr<Time> runTime
    )
    :
    BaseMultiLevelSolver( 5, 3 ),
    name( name ),
    args( args ),
    runTime( runTime ),
    meshPtr
    (
        dynamicFvMesh::New
        (
            IOobject
            (
                name,
                runTime->timeName(),
                *runTime,
                IOobject::MUST_READ
            )
        )
    ),
    mesh( meshPtr() ),
    couplingProperties
    (
        IOobject
        (
            "couplingProperties",
            runTime->constant(),
            mesh,
            IOobject::MUST_READ,
            IOobject::NO_WRITE
        )
    ),
    movingPatches( couplingProperties.lookup( "movingFluidPatches" ) ),
    movingPatchIDs( movingPatches.size() ),
    movingPatchesDispl( mesh.boundaryMesh().size(), vectorField( 0 ) ),
    movingPatchesDisplOld( mesh.boundaryMesh().size(), vectorField( 0 ) ),
    nPoints( Pstream::nProcs(), 0 ),
    nGlobalPoints( Pstream::nProcs(), 0 ),
    nGlobalCenters( Pstream::nProcs(), 0 ),
    globalPointsUnique( 1, -1 ),
    globalPointsNonUnique( 1, -1 ),
    totalRunTime( 0 ),
    totalNbIterations( 0 ),
    twoDCorrector( mesh ),
    nbGlobalPoints( Pstream::nProcs(), 0 )
{
    // Find IDs of staticPatches_
    forAll( movingPatches, patchI )
    {
        label patchIndex = mesh.boundaryMesh().findPatchID( movingPatches[patchI] );

        assert( patchIndex >= 0 );

        movingPatchIDs[patchI] = patchIndex;
    }

    forAll( movingPatchIDs, patchI )
    {
        int size = mesh.boundaryMesh()[movingPatchIDs[patchI]].meshPoints().size();

        movingPatchesDispl[movingPatchIDs[patchI]] = vectorField( size, Foam::vector::zero );
        movingPatchesDisplOld[movingPatchIDs[patchI]] = vectorField( size, Foam::vector::zero );
    }

    matrix writePositions;
    getWritePositions( writePositions );

    N = writePositions.rows();
    dim = mesh.nGeometricD();
    data.resize( N, dim );
    data.setZero();

    assert( readLabel( runTime->controlDict().lookup( "writePrecision" ) ) >= 12 );
    timeIndex = runTime->timeIndex();
}

foamFluidSolver::~foamFluidSolver()
{}

void foamFluidSolver::finalizeTimeStep()
{
    assert( init );

    if ( runTime->outputTime() && name == Foam::fvMesh::defaultRegion )
        runTime->write();

    init = false;
}

void foamFluidSolver::getReadPositions( matrix & readPositions )
{
    // Read positions: the face vertices of the moving patches

    std::unordered_map<unsigned int, std::unordered_map<std::string, unsigned int> > movingPoints;
    std::vector<unsigned int> movingPointLabels;

    globalMovingPoints.clear();
    globalMovingPointLabels.clear();

    forAll( movingPatchIDs, patchI )
    {
        const labelList & meshPoints = mesh.boundaryMesh()[movingPatchIDs[patchI]].meshPoints();

        forAll( meshPoints, j )
        {
            if ( twoDCorrector.marker()[meshPoints[j]] != 0 )
                continue;

            if ( movingPoints.find( meshPoints[j] ) == movingPoints.end() )
            {
                std::unordered_map<std::string, unsigned int> movingPoint;
                movingPoint["patch-id"] = movingPatchIDs[patchI];
                movingPoint["point-index"] = j;
                movingPoint["local-id"] = movingPoints.size();
                movingPoint["local-id-unique"] = movingPoint["local-id"];
                movingPoint["mesh-index"] = meshPoints[j];
                movingPoints[meshPoints[j]] = movingPoint;
                movingPointLabels.push_back( meshPoints[j] );
            }
        }
    }

    assert( movingPointLabels.size() == movingPoints.size() );

    if ( Pstream::nProcs() > 1 )
    {
        IOobject addrHeader
        (
            "pointProcAddressing",
            mesh.facesInstance(),
            mesh.meshSubDir,
            mesh,
            IOobject::MUST_READ
        );

        assert( addrHeader.headerOk() );
        labelIOList pointProcAddressing( addrHeader );

        assert( pointProcAddressing.size() == mesh.points().size() );

        nbGlobalPoints = 0;
        nbGlobalPoints[Pstream::myProcNo()] = movingPointLabels.size();
        reduce( nbGlobalPoints, sumOp<labelList>() );

        // Construct a list with all the global point labels
        // Thereafter, construct a list which indicates whether the point is
        // already included or not. Use this later to build a list of the
        // unique points.
        labelList globalPointsIndices( sum( nbGlobalPoints ), 0 );

        int globalOffsetNonUnique = 0;

        for ( int i = 0; i < Pstream::myProcNo(); i++ )
            globalOffsetNonUnique += nbGlobalPoints[i];

        for ( auto label : movingPoints )
        {
            globalPointsIndices[label.second["local-id"] + globalOffsetNonUnique] = pointProcAddressing[label.first];
        }

        reduce( globalPointsIndices, sumOp<labelList>() );

        // Construct unique list with global indices

        int localIdUnique = 0;

        forAll( globalPointsIndices, i )
        {
            if ( globalMovingPoints.find( globalPointsIndices[i] ) == globalMovingPoints.end() )
            {
                std::unordered_map<std::string, unsigned int> movingPoint;
                movingPoint["global-id"] = globalMovingPoints.size();
                int localId = i - globalOffsetNonUnique;

                if ( localId >= 0
                    && !movingPoints.empty()
                    && localId < static_cast<int>( movingPoints.size() ) )
                {
                    movingPoint["local-point"] = true;
                    movingPoint["mesh-index"] = movingPointLabels[localId];
                    movingPoint["local-id"] = localId;
                    movingPoint["local-id-unique"] = localIdUnique;
                    movingPoint["patch-id"] = movingPoints[movingPoint["mesh-index"]]["patch-id"];
                    movingPoint["point-index"] = movingPoints[movingPoint["mesh-index"]]["point-index"];

                    localIdUnique++;
                }
                else
                {
                    movingPoint["local-point"] = false;
                }

                globalMovingPoints[globalPointsIndices[i]] = movingPoint;
                globalMovingPointLabels.push_back( globalPointsIndices[i] );
            }
        }
    }
    else
    {
        for ( auto point : movingPoints )
        {
            assert( point.second.find( "mesh-index" ) != point.second.end() );

            point.second["local-point"] = true;
            point.second["global-id"] = point.second["local-id"];
            globalMovingPoints[point.first] = point.second;
        }

        for ( auto label : movingPointLabels )
        {
            globalMovingPointLabels.push_back( label );
        }
    }

    vectorField positionsField( globalMovingPoints.size(), Foam::vector::zero );

    for ( auto point : globalMovingPoints )
    {
        assert( point.second.find( "local-point" ) != point.second.end() );

        if ( point.second["local-point"] == true )
        {
            assert( point.second.find( "global-id" ) != point.second.end() );
            assert( point.second.find( "mesh-index" ) != point.second.end() );

            for ( int j = 0; j < mesh.nGeometricD(); j++ )
            {
                positionsField[point.second["global-id"]][j] = mesh.points()[point.second["mesh-index"]][j];
            }
        }
    }

    reduce( positionsField, sumOp<vectorField>() );

    readPositions.resize( positionsField.size(), mesh.nGeometricD() );

    for ( int i = 0; i < readPositions.rows(); i++ )
        for ( int j = 0; j < readPositions.cols(); j++ )
            readPositions( i, j ) = positionsField[i][j];
}

void foamFluidSolver::getReadPositionsLocal( matrix & readPositionsLocal )
{
    matrix readPositions;
    getReadPositions( readPositions );

    assert( readPositions.rows() == static_cast<int>( globalMovingPoints.size() ) );

    unsigned int N = 0;

    for ( auto point : globalMovingPoints )
    {
        assert( point.second.find( "local-point" ) != point.second.end() );

        if ( point.second["local-point"] == true )
            N++;
    }

    readPositionsLocal.resize( N, mesh.nGeometricD() );

    for ( auto && point : globalMovingPoints )
    {
        assert( point.second.find( "local-point" ) != point.second.end() );

        if ( point.second["local-point"] == true )
        {
            assert( point.second.find( "mesh-index" ) != point.second.end() );
            assert( point.second.find( "local-id-unique" ) != point.second.end() );

            for ( int j = 0; j < mesh.nGeometricD(); j++ )
            {
                readPositionsLocal( point.second["local-id-unique"], j ) = mesh.points()[point.second["mesh-index"]][j];
            }
        }
    }
}

void foamFluidSolver::getWritePositions( matrix & writePositions )
{
    int size = 0;

    forAll( movingPatchIDs, patchI )
    {
        size += mesh.boundaryMesh()[movingPatchIDs[patchI]].faceCentres().size();
    }

    nGlobalCenters = 0;
    nGlobalCenters[Pstream::myProcNo()] = size;
    reduce( nGlobalCenters, sumOp<labelList>() );

    vectorField writePositionsField( sum( nGlobalCenters ), Foam::vector::zero );

    int globalOffset = 0;

    for ( int i = 0; i < Pstream::myProcNo(); i++ )
        globalOffset += nGlobalCenters[i];

    matrix writePositionsLocal;
    getWritePositionsLocal( writePositionsLocal );

    for ( int i = 0; i < writePositionsLocal.rows(); i++ )
        for ( int j = 0; j < writePositionsLocal.cols(); j++ )
            writePositionsField[i + globalOffset][j] = writePositionsLocal( i, j );

    reduce( writePositionsField, sumOp<vectorField>() );

    writePositions.resize( writePositionsField.size(), mesh.nGeometricD() );

    for ( int i = 0; i < writePositions.rows(); i++ )
        for ( int j = 0; j < writePositions.cols(); j++ )
            writePositions( i, j ) = writePositionsField[i][j];
}

void foamFluidSolver::getWritePositionsLocal( matrix & writePositions )
{
    int size = 0;

    forAll( movingPatchIDs, patchI )
    {
        size += mesh.boundaryMesh()[movingPatchIDs[patchI]].faceCentres().size();
    }

    writePositions.resize( size, mesh.nGeometricD() );

    int offset = 0;

    forAll( movingPatchIDs, patchI )
    {
        const vectorField faceCentres( mesh.boundaryMesh()[movingPatchIDs[patchI]].faceCentres() );

        for ( int i = 0; i < faceCentres.size(); i++ )
            for ( int j = 0; j < writePositions.cols(); j++ )
                writePositions( i + offset, j ) = faceCentres[i][j];

        offset += faceCentres.size();
    }
}

void foamFluidSolver::moveMesh()
{
    bool elRBFSolver = mesh.objectRegistry::foundObject<ElRBFMeshMotionSolver>( "dynamicMeshDict" );

    if ( elRBFSolver )
    {
        ElRBFMeshMotionSolver & motionSolver =
            const_cast<ElRBFMeshMotionSolver &>
            (
            mesh.lookupObject<ElRBFMeshMotionSolver>( "dynamicMeshDict" )
            );

        motionSolver.setMotion( movingPatchesDispl - movingPatchesDisplOld );
    }
    else
    {
        RBFMeshMotionSolver & motionSolver =
            const_cast<RBFMeshMotionSolver &>
            (
            mesh.lookupObject<RBFMeshMotionSolver>( "dynamicMeshDict" )
            );

        motionSolver.setMotion( movingPatchesDispl - movingPatchesDisplOld );
    }
}

void foamFluidSolver::setDisplacementLocal( const matrix & displacement )
{
    movingPatchesDisplOld = movingPatchesDispl;

    for ( auto point : globalMovingPoints )
    {
        assert( point.second.find( "local-point" ) != point.second.end() );

        if ( point.second["local-point"] == true )
        {
            assert( point.second.find( "patch-id" ) != point.second.end() );
            assert( point.second.find( "point-index" ) != point.second.end() );
            assert( point.second.find( "local-id-unique" ) != point.second.end() );

            int patchId = point.second["patch-id"];
            int pointIndex = point.second["point-index"];
            int localId = point.second["local-id-unique"];

            for ( int j = 0; j < mesh.nGeometricD(); j++ )
            {
                movingPatchesDispl[patchId][pointIndex][j] = displacement( localId, j );
            }
        }
    }
}

void foamFluidSolver::run()
{
    while ( isRunning() )
    {
        initTimeStep();
        solve();
        finalizeTimeStep();
    }
}

void foamFluidSolver::solve(
    const matrix & input,
    matrix & output
    )
{
    movingPatchesDisplOld = movingPatchesDispl;

    assert( input.rows() == static_cast<int>( globalMovingPointLabels.size() ) );
    assert( globalMovingPoints.size() == globalMovingPointLabels.size() );
    assert( input.cols() == mesh.nGeometricD() );

    for ( auto point : globalMovingPoints )
    {
        assert( point.second.find( "local-point" ) != point.second.end() );

        if ( point.second["local-point"] == true )
        {
            assert( point.second.find( "patch-id" ) != point.second.end() );
            assert( point.second.find( "point-index" ) != point.second.end() );
            assert( point.second.find( "local-id-unique" ) != point.second.end() );
            assert( point.second.find( "global-id" ) != point.second.end() );

            int patchId = point.second["patch-id"];
            int pointIndex = point.second["point-index"];
            int globalId = point.second["global-id"];

            assert( patchId < movingPatchesDispl.size() );
            assert( pointIndex < movingPatchesDispl[patchId].size() );
            assert( globalId < input.rows() );

            for ( int j = 0; j < mesh.nGeometricD(); j++ )
            {
                movingPatchesDispl[patchId][pointIndex][j] = input( globalId, j );
            }
        }
    }

    moveMesh();

    solve();

    matrix traction;
    getTractionLocal( traction );

    assert( traction.rows() == nGlobalCenters[Pstream::myProcNo()] );
    assert( traction.cols() == mesh.nGeometricD() );

    vectorField outputField( sum( nGlobalCenters ), Foam::vector::zero );

    int globalOffset = 0;

    for ( int i = 0; i < Pstream::myProcNo(); i++ )
        globalOffset += nGlobalCenters[i];

    for ( int i = 0; i < traction.rows(); i++ )
        for ( int j = 0; j < traction.cols(); j++ )
            outputField[i + globalOffset][j] = traction( i, j );

    reduce( outputField, sumOp<vectorField>() );

    output.resize( outputField.size(), mesh.nGeometricD() );

    for ( int i = 0; i < output.rows(); i++ )
        for ( int j = 0; j < output.cols(); j++ )
            output( i, j ) = outputField[i][j];

    data = output;
}
