/*---------------------------------------------------------------------------*\
  =========                 |
  \\      /  F ield         | OpenFOAM: The Open Source CFD Toolbox
   \\    /   O peration     |
    \\  /    A nd           | www.openfoam.com
     \\/     M anipulation  |
-------------------------------------------------------------------------------
    Copyright (C) 2019-2020 DLR
-------------------------------------------------------------------------------
License
    This file is part of OpenFOAM.

    OpenFOAM is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    OpenFOAM is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
    for more details.

    You should have received a copy of the GNU General Public License
    along with OpenFOAM.  If not, see <http://www.gnu.org/licenses/>.

\*---------------------------------------------------------------------------*/

#include "isoAlpha.H"
#include "addToRunTimeSelectionTable.H"
#include "cutCellPLIC.H"
#include "OBJstream.H"

// * * * * * * * * * * * * * * Static Data Members * * * * * * * * * * * * * //

namespace Foam
{
namespace reconstruction
{
    defineTypeNameAndDebug(isoAlpha, 0);
    addToRunTimeSelectionTable(reconstructionSchemes,isoAlpha, components);
}
}


// * * * * * * * * * * * * * * * * Constructors  * * * * * * * * * * * * * * //

Foam::reconstruction::isoAlpha::isoAlpha
(
    volScalarField& alpha1,
    const surfaceScalarField& phi,
    const volVectorField& U,
    const dictionary& dict
)
:
    reconstructionSchemes
    (
        typeName,
        alpha1,
        phi,
        U,
        dict
    ),
    mesh_(alpha1.mesh()),
    // Interpolation data
    ap_(mesh_.nPoints()),

    // Tolerances and solution controls
    isoFaceTol_(modelDict().lookupOrDefault<scalar>("isoFaceTol", 1e-8)),
    surfCellTol_(modelDict().lookupOrDefault<scalar>("surfCellTol", 1e-8)),
    sIterIso_(mesh_,ap_,surfCellTol_)
{
    reconstruct();
}


// * * * * * * * * * * * * * * Member Functions  * * * * * * * * * * * * * * //

void Foam::reconstruction::isoAlpha::reconstruct(bool forceUpdate)
{
    const bool uptodate = alreadyReconstructed(forceUpdate);

    if (uptodate && !forceUpdate)
    {
        return;
    }

    // Interpolating alpha1 cell centre values to mesh points (vertices)
    if (mesh_.topoChanging())
    {
        // Introduced resizing to cope with changing meshes
        if (ap_.size() != mesh_.nPoints())
        {
            ap_.resize(mesh_.nPoints());

        }
        if (interfaceCell_.size() != mesh_.nCells())
        {
            interfaceCell_.resize(mesh_.nCells());
        }
    }
    ap_ = volPointInterpolation::New(mesh_).interpolate(alpha1_);

    DynamicList<List<point>> facePts;

    interfaceLabels_.clear();

    scalar area = 0;

    forAll(alpha1_,cellI)
    {
        if (sIterIso_.isASurfaceCell(alpha1_[cellI]))
        {
            interfaceLabels_.append(cellI);

            sIterIso_.vofCutCell
            (
                cellI,
                alpha1_[cellI],
                isoFaceTol_,
                100
            );

            if (sIterIso_.cellStatus() == 0)
            {
                normal_[cellI] = sIterIso_.surfaceArea();
                centre_[cellI] = sIterIso_.surfaceCentre();
                if (mag(normal_[cellI]) != 0)
                {
                    interfaceCell_[cellI] = true;
                    facePts.append(sIterIso_.facePoints());
                    area += mag(normal_[cellI]);
                }
                else
                {
                    interfaceCell_[cellI] = false;
                  //facePts.append(sIterIso_.facePoints());
                }

            }
            else
            {
                normal_[cellI] = Zero;
                centre_[cellI] = Zero;
                interfaceCell_[cellI] = false;
            }
         }
         else
         {
            normal_[cellI] = Zero;
            centre_[cellI] = Zero;
            interfaceCell_[cellI] = false;
         }
    }
    Info<<"Area :"<<area<<endl;
    writeIsoFaces(facePts);
}

void Foam::reconstruction::isoAlpha::writeIsoFaces
(
    DynamicList<List<point>>& faces
)
{

    //bool writeIsoFaces = modelDict().readIfPresent("writeIsoFaces", True);
    //if(writeIsoFaces && mesh_.time().writeTime())
    if(mesh_.time().writeTime())
    {
        // Writing isofaces to obj file for inspection, e.g. in paraview
        const fileName dirName
        (
            Pstream::parRun() ?
                mesh_.time().path()/".."/"isoFaces"
              : mesh_.time().path()/"isoFaces"
        );
        const string fName
        (
            "isoFaces_" + alpha1_.name() + Foam::name(mesh_.time().timeIndex())
            // Changed because only OF+ has two parameter version of Foam::name
            // "isoFaces_" + Foam::name("%012d", mesh_.time().timeIndex())
        );

        if (Pstream::parRun())
        {
            // Collect points from all the processors
            List<DynamicList<List<point> > > allProcFaces(Pstream::nProcs());
            allProcFaces[Pstream::myProcNo()] = faces;
            Pstream::gatherList(allProcFaces);

            if (Pstream::master())
            {
                mkDir(dirName);
                OBJstream os(dirName/fName + ".obj");
                Info<< nl << "isoAlpha: writing iso faces to file: "
                    << os.name() << nl << endl;

                face f;
                forAll(allProcFaces, proci)
                {
                    const DynamicList<List<point> >& procFacePts =
                        allProcFaces[proci];

                    forAll(procFacePts, i)
                    {
                        const List<point>& facePts = procFacePts[i];

                        if (facePts.size() != f.size())
                        {
                            f = face(identity(facePts.size()));
                        }

                        os.write(f, facePts, false);
                    }
                }
            }
        }
        else
        {
            mkDir(dirName);
            OBJstream os(dirName/fName + ".obj");
            Info<< nl << "isoAlpha: writing iso faces to file: "
                << os.name() << nl << endl;

            face f;
            forAll(faces, i)
            {
                const List<point>& facePts = faces[i];

                if (facePts.size() != f.size())
                {
                    f = face(identity(facePts.size()));
                }

                os.write(f, facePts, false);
            }
        }
    }
}

// ************************************************************************* //
