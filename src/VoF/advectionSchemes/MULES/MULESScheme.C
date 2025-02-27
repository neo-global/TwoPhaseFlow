/*---------------------------------------------------------------------------*\
  =========                 |
  \\      /  F ield         | OpenFOAM: The Open Source CFD Toolbox
   \\    /   O peration     |
    \\  /    A nd           | Copyright (C) 2011 OpenFOAM Foundation
     \\/     M anipulation  |
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


#include "MULESScheme.H"
#include "addToRunTimeSelectionTable.H"
#include "fvCFD.H"
#include "CMULES.H"
#include "EulerDdtScheme.H"
#include "localEulerDdtScheme.H"
#include "CrankNicolsonDdtScheme.H"
#include "subCycle.H"

#include "fvcSurfaceIntegrate.H"
#include "upwind.H"



// * * * * * * * * * * * * * * Static Data Members * * * * * * * * * * * * * //

namespace Foam
{
namespace advection
{
    defineTypeNameAndDebug(MULESScheme, 0);
    addToRunTimeSelectionTable(advectionSchemes,MULESScheme, components);
}
}


void Foam::advection::MULESScheme::updateNHatf()
{
    // Cell gradient of alpha
    const volVectorField gradAlpha(fvc::grad(alpha1_, "nHat"));

    // Interpolated face-gradient of alpha
    surfaceVectorField gradAlphaf(fvc::interpolate(gradAlpha));

    //gradAlphaf -=
    //    (mesh.Sf()/mesh.magSf())
    //   *(fvc::snGrad(alpha1_) - (mesh.Sf() & gradAlphaf)/mesh.magSf());

        // Face unit interface normal
    surfaceVectorField nHatfv(gradAlphaf/(mag(gradAlphaf) + deltaN_));

    nHatf_ = nHatfv & mesh_.Sf();
}


// * * * * * * * * * * * * * * * * Constructors  * * * * * * * * * * * * * * //

Foam::advection::MULESScheme::MULESScheme
(
        volScalarField& alpha1,
        const surfaceScalarField& phi,
        const volVectorField& U
)
:
    advectionSchemes
    (
        typeName,
        alpha1,
        phi,
        U
    ),
    mesh_(alpha1.mesh()),
    alpha2_(1-alpha1),
    talphaPhiCorr0_
    (
         IOobject
         (
             "talphaPhiCorr0_",
             mesh_.time().timeName(),
             mesh_,
             IOobject::READ_IF_PRESENT,
             IOobject::AUTO_WRITE
         ),
         phi_*fvc::interpolate(alpha1_)
    ),
    deltaN_
     (
         "deltaN",
         1e-8/pow(average(alpha1.mesh().V()), 1.0/3.0)
     ),
     nHatf_
     (
         IOobject
         (
             "nHatf",
             alpha1_.time().timeName(),
             alpha1_.mesh()
         ),
         alpha1_.mesh(),
         dimensionedScalar("nHatf", dimArea, 0.0)
     )





{

    //- Reference to mesh
    LTS_ = fv::localEulerDdt::enabled(mesh_);
    const dictionary& alphaControls = mesh_.solverDict(alpha1_.name());;

    nAlphaCorr_= (readLabel(alphaControls.lookup("nAlphaCorr")));

    nAlphaSubCycles_ = (readLabel(alphaControls.lookup("nAlphaSubCycles")));

    MULESCorr_ = (alphaControls.lookupOrDefault<Switch>("MULESCorr", false));

// Apply the compression correction from the previous iteration
// Improves efficiency for steady-simulations but can only be applied
// once the alpha field is reasonably steady, i.e. fully developed
    alphaApplyPrevCorr_ =
     (
         alphaControls.lookupOrDefault<Switch>("alphaApplyPrevCorr", false)
     );
 //Isotropic compression coefficient
    icAlpha_ =
     (
         alphaControls.lookupOrDefault<scalar>("icAlpha", 0)
     );

    cAlpha_ = readScalar(alphaControls.lookup("cAlpha"));


}

// * * * * * * * * * * * * * * * * Destructor  * * * * * * * * * * * * * * * //

Foam::advection::MULESScheme::~MULESScheme()
{}

// * * * * * * * * * * * * * * Protected Access Member Functions  * * * * * * * * * * * * * * //

// * * * * * * * * * * * * * * Member Functions  * * * * * * * * * * * * * * //


// ************************************************************************* //
void Foam::advection::MULESScheme::advect()
{
    updateNHatf();

    word alphaScheme("div(phi,alpha)");
    word alpharScheme("div(phirb,alpha)");

    scalar ocCoeff = 0;
    {

        tmp<fv::ddtScheme<scalar>> ddtAlpha
        (
            fv::ddtScheme<scalar>::New
            (
                mesh_,
                mesh_.ddtScheme("ddt(alpha)")
            )
        );

        // Set the off-centering coefficient according to ddt scheme
        if
        (
            isType<fv::EulerDdtScheme<scalar>>(ddtAlpha())
         || isType<fv::localEulerDdtScheme<scalar>>(ddtAlpha())
        )
        {
            ocCoeff = 0;
        }
        else if (isType<fv::CrankNicolsonDdtScheme<scalar>>(ddtAlpha()))
        {
            if (nAlphaSubCycles_ > 1)
            {
                FatalErrorInFunction
                    << "Sub-cycling is not supported "
                       "with the CrankNicolson ddt scheme"
                    << exit(FatalError);
            }

            ocCoeff =
                refCast<const fv::CrankNicolsonDdtScheme<scalar>>(ddtAlpha())
               .ocCoeff();
        }
        else
        {
            FatalErrorInFunction
                << "Only Euler and CrankNicolson ddt schemes are supported"
                << exit(FatalError);
        }
    }

    scalar cnCoeff = 1.0/(1.0 + ocCoeff);

    // Standard face-flux compression coefficient
    surfaceScalarField phic(cAlpha_*mag(phi_/mesh_.magSf()));

    // Add the optional isotropic compression contribution
    if (icAlpha_ > 0)
    {
        phic *= (1.0 - icAlpha_);
        phic += (cAlpha_*icAlpha_)*fvc::interpolate(mag(U_));
    }

    surfaceScalarField::Boundary& phicBf =
        phic.boundaryFieldRef();

    // Do not compress interface at non-coupled boundary faces
    // (inlets, outlets etc.)
    forAll(phic.boundaryField(), patchi)
    {
        fvsPatchScalarField& phicp = phicBf[patchi];

        if (!phicp.coupled())
        {
            phicp == 0;
        }
    }

    tmp<surfaceScalarField> phiCN(phi_);

    // Calculate the Crank-Nicolson off-centred volumetric flux
    if (ocCoeff > 0)
    {
        phiCN = cnCoeff*phi_ + (1.0 - cnCoeff)*phi_.oldTime();
    }

    if (MULESCorr_)
    {
        fvScalarMatrix alpha1Eqn
        (
            (
                LTS_
              ? fv::localEulerDdtScheme<scalar>(mesh_).fvmDdt(alpha1_)
              : fv::EulerDdtScheme<scalar>(mesh_).fvmDdt(alpha1_)
            )
          + fv::gaussConvectionScheme<scalar>
            (
                mesh_,
                phiCN,
                upwind<scalar>(mesh_, phiCN)
            ).fvmDiv(phiCN, alpha1_)
        );

        alpha1Eqn.solve();

        Info<< "Phase-1 volume fraction = "
            << alpha1_.weightedAverage(mesh_.Vsc()).value()
            << "  Min(" << alpha1_.name() << ") = " << min(alpha1_).value()
            << "  Max(" << alpha1_.name() << ") = " << max(alpha1_).value()
            << endl;

        tmp<surfaceScalarField> talphaPhiUD(alpha1Eqn.flux());
        alphaPhi_ = talphaPhiUD();

        if (alphaApplyPrevCorr_)
        {
            Info<< "Applying the previous iteration compression flux" << endl;
            MULES::correct
            (
                geometricOneField(),
                alpha1_,
                alphaPhi_,
                talphaPhiCorr0_,
                oneField(),
                zeroField()
            );



            alphaPhi_ += talphaPhiCorr0_;
        }

        // Cache the upwind-flux
        talphaPhiCorr0_ = talphaPhiUD;

        alpha2_ = 1.0 - alpha1_;

       // mixture.correct();
        updateNHatf();
    }


    for (int aCorr=0; aCorr<nAlphaCorr_; aCorr++)
    {
        surfaceScalarField phir(phic*nHatf_);

        tmp<surfaceScalarField> talphaPhiUn
            (
                fvc::flux
                (
                    phi_,
                    alpha1_,
                    alphaScheme
                )
            + fvc::flux
                (
                    -fvc::flux(-phir, alpha2_, alpharScheme),
                    alpha1_,
                    alpharScheme
                )
            );

        // Calculate the Crank-Nicolson off-centred alpha flux
//            if (ocCoeff > 0)
//            {
//                talphaPhiUn =
//                    cnCoeff*talphaPhiUn + (1.0 - cnCoeff)*alphaPhi_.oldTime();
//            }

        if (MULESCorr_)
        {
            tmp<surfaceScalarField> talphaPhiCorr(talphaPhiUn - alphaPhi_);
            volScalarField alpha10("alpha10", alpha1_);

            //MULES::correct(alpha1_, talphaPhiUn, talphaPhiCorr.ref(), 1, 0);
            MULES::correct
            (
                geometricOneField(),
                alpha1_,
                alphaPhi_,
                talphaPhiCorr0_,
                oneField(),
                zeroField()
            );
            // Under-relax the correction for all but the 1st corrector
            if (aCorr == 0)
            {
                alphaPhi_ += talphaPhiCorr();
            }
            else
            {
                alpha1_ = 0.5*alpha1_ + 0.5*alpha10;
                alphaPhi_ += 0.5*talphaPhiCorr();
            }
        }
        else
        {
            alphaPhi_ = talphaPhiUn;

            MULES::explicitSolve
            (
                geometricOneField(),
                alpha1_,
                phiCN,
                alphaPhi_,
                1,
                0
            );
        }

        alpha2_ = 1.0 - alpha1_;

        // mixture.correct();
         updateNHatf();
    }

    if (alphaApplyPrevCorr_ && MULESCorr_)
    {
        talphaPhiCorr0_ = alphaPhi_ - talphaPhiCorr0_;
    }

    if
    (
        word(mesh_.ddtScheme("ddt(rho,U)"))
     == fv::EulerDdtScheme<vector>::typeName
    )
    {
//            rhoPhi = alphaPhi_*(rho1 - rho2) + phiCN*rho2;
    }
    else
    {
        if (ocCoeff > 0)
        {
            // Calculate the end-of-time-step alpha flux
            alphaPhi_ = (alphaPhi_ - (1.0 - cnCoeff)*alphaPhi_.oldTime())/cnCoeff;
        }

        // Calculate the end-of-time-step mass flux
        //rhoPhi = alphaPhi_*(rho1 - rho2) + phi_*rho2;
    }

    Info<< "Phase-1 volume fraction = "
        << alpha1_.weightedAverage(mesh_.Vsc()).value()
        << "  Min(" << alpha1_.name() << ") = " << min(alpha1_).value()
        << "  Max(" << alpha1_.name() << ") = " << max(alpha1_).value()
        << endl;
}

void Foam::advection::MULESScheme::advect(const volScalarField::Internal& Sp,const volScalarField::Internal& Su)
{
    updateNHatf();

    word alphaScheme("div(phi,alpha)");
    word alpharScheme("div(phirb,alpha)");

    scalar ocCoeff = 0;
    {

        tmp<fv::ddtScheme<scalar>> ddtAlpha
        (
            fv::ddtScheme<scalar>::New
            (
                mesh_,
                mesh_.ddtScheme("ddt(alpha)")
            )
        );

        // Set the off-centering coefficient according to ddt scheme

        if
        (
            isType<fv::EulerDdtScheme<scalar>>(ddtAlpha())
         || isType<fv::localEulerDdtScheme<scalar>>(ddtAlpha())
        )
        {
            ocCoeff = 0;
        }
        else if (isType<fv::CrankNicolsonDdtScheme<scalar>>(ddtAlpha()))
        {
            if (nAlphaSubCycles_ > 1)
            {
                FatalErrorInFunction
                    << "Sub-cycling is not supported "
                       "with the CrankNicolson ddt scheme"
                    << exit(FatalError);
            }

            ocCoeff =
                refCast<const fv::CrankNicolsonDdtScheme<scalar>>(ddtAlpha())
               .ocCoeff();
        }
        else
        {
            FatalErrorInFunction
                << "Only Euler and CrankNicolson ddt schemes are supported"
                << exit(FatalError);
        }
    }

    scalar cnCoeff = 1.0/(1.0 + ocCoeff);

    // Standard face-flux compression coefficient
    surfaceScalarField phic(cAlpha_*mag(phi_/mesh_.magSf()));

    // Add the optional isotropic compression contribution
    if (icAlpha_ > 0)
    {
        phic *= (1.0 - icAlpha_);
        phic += (cAlpha_*icAlpha_)*fvc::interpolate(mag(U_));
    }

    surfaceScalarField::Boundary& phicBf =
        phic.boundaryFieldRef();

    // Do not compress interface at non-coupled boundary faces
    // (inlets, outlets etc.)
    forAll(phic.boundaryField(), patchi)
    {
        fvsPatchScalarField& phicp = phicBf[patchi];

        if (!phicp.coupled())
        {
            phicp == 0;
        }
    }

    tmp<surfaceScalarField> phiCN(phi_);

    // Calculate the Crank-Nicolson off-centred volumetric flux
    if (ocCoeff > 0)
    {
        phiCN = cnCoeff*phi_ + (1.0 - cnCoeff)*phi_.oldTime();
    }

    if (MULESCorr_)
    {
        fvScalarMatrix alpha1Eqn
        (
            (
                LTS_
              ? fv::localEulerDdtScheme<scalar>(mesh_).fvmDdt(alpha1_)
              : fv::EulerDdtScheme<scalar>(mesh_).fvmDdt(alpha1_)
            )
          + fv::gaussConvectionScheme<scalar>
            (
                mesh_,
                phiCN,
                upwind<scalar>(mesh_, phiCN)
            ).fvmDiv(phiCN, alpha1_)
             ==
                 Su + fvm::Sp(Sp , alpha1_)
        );

        alpha1Eqn.solve();

        Info<< "Phase-1 volume fraction = "
            << alpha1_.weightedAverage(mesh_.Vsc()).value()
            << "  Min(" << alpha1_.name() << ") = " << min(alpha1_).value()
            << "  Max(" << alpha1_.name() << ") = " << max(alpha1_).value()
            << endl;

        tmp<surfaceScalarField> talphaPhiUD(alpha1Eqn.flux());
        alphaPhi_ = talphaPhiUD();

        if (alphaApplyPrevCorr_)
        {
            Info<< "Applying the previous iteration compression flux" << endl;
            //MULES::correct(alpha1_, alphaPhi_, talphaPhiCorr0_, 1, 0);

            MULES::correct
            (
                geometricOneField(),
                alpha1_,
                alphaPhi_,
                talphaPhiCorr0_,
                oneField(),
                zeroField()
            );

            alphaPhi_ += talphaPhiCorr0_;
        }

        // Cache the upwind-flux
        talphaPhiCorr0_ = talphaPhiUD;

        alpha2_ = 1.0 - alpha1_;

       // mixture.correct();
        updateNHatf();
    }


    for (int aCorr=0; aCorr<nAlphaCorr_; aCorr++)
    {
        surfaceScalarField phir(phic*nHatf_);

        tmp<surfaceScalarField> talphaPhiUn
            (
                fvc::flux
                (
                    phi_,
                    alpha1_,
                    alphaScheme
                )
            + fvc::flux
                (
                    -fvc::flux(-phir, alpha2_, alpharScheme),
                    alpha1_,
                    alpharScheme
                )
            );

        // Calculate the Crank-Nicolson off-centred alpha flux
//            if (ocCoeff > 0)
//            {
//                talphaPhiUn =
//                    cnCoeff*talphaPhiUn + (1.0 - cnCoeff)*alphaPhi_.oldTime();
//            }

        if (MULESCorr_)
        {
            tmp<surfaceScalarField> talphaPhiCorr(talphaPhiUn - alphaPhi_);
            volScalarField alpha10("alpha10", alpha1_);

            MULES::correct
            (
                geometricOneField(),
                alpha1_,
                phiCN,
                alphaPhi_,
                Sp,
                Su,
                1,
                0
            );

            // Under-relax the correction for all but the 1st corrector
            if (aCorr == 0)
            {
                alphaPhi_ += talphaPhiCorr();
            }
            else
            {
                alpha1_ = 0.5*alpha1_ + 0.5*alpha10;
                alphaPhi_ += 0.5*talphaPhiCorr();
            }
        }
        else
        {

            alphaPhi_ = talphaPhiUn;


            MULES::explicitSolve
            (
               geometricOneField(),
               alpha1_,
               phiCN,
               alphaPhi_,
               Sp,
               Su,
               1,
               0
            );
        }

        alpha2_ = 1.0 - alpha1_;

        // mixture.correct();
         updateNHatf();
    }

    if (alphaApplyPrevCorr_ && MULESCorr_)
    {
        talphaPhiCorr0_ = alphaPhi_ - talphaPhiCorr0_;
    }

    if
    (
        word(mesh_.ddtScheme("ddt(rho,U)"))
     == fv::EulerDdtScheme<vector>::typeName
    )
    {
//            rhoPhi = alphaPhi_*(rho1 - rho2) + phiCN*rho2;
    }
    else
    {
        if (ocCoeff > 0)
        {
            // Calculate the end-of-time-step alpha flux
            alphaPhi_ = (alphaPhi_ - (1.0 - cnCoeff)*alphaPhi_.oldTime())/cnCoeff;
        }

        // Calculate the end-of-time-step mass flux
        //rhoPhi = alphaPhi_*(rho1 - rho2) + phi_*rho2;
    }

    Info<< "Phase-1 volume fraction = "
        << alpha1_.weightedAverage(mesh_.Vsc()).value()
        << "  Min(" << alpha1_.name() << ") = " << min(alpha1_).value()
        << "  Max(" << alpha1_.name() << ") = " << max(alpha1_).value()
        << endl;

}
