/*---------------------------------------------------------------------------*\
  =========                 |
  \\      /  F ield         | OpenFOAM: The Open Source CFD Toolbox
   \\    /   O peration     |
    \\  /    A nd           | Copyright (C) 2011-2012 OpenFOAM Foundation
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

#include "mutUSpaldingWallFunctionFvPatchScalarField.H"
#include "fvPatchFieldMapper.H"
#include "volFields.H"
#include "compressible/turbulenceModel/turbulenceModel.H"
#include "addToRunTimeSelectionTable.H"

// * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * //

namespace Foam
{
namespace compressible
{

struct mutUSpaldingCalcUTauFunctor
{
    const scalar kappa;
    const scalar E;

    mutUSpaldingCalcUTauFunctor(const scalar kappa_,const scalar E_):
                                kappa(kappa_),E(E_){}
    __HOST____DEVICE__
    scalar operator () (const scalar& y, const thrust::tuple<scalar,scalar,scalar,scalar,scalar>& t)
    {
        scalar muw = thrust::get<0>(t);
        scalar mutw = thrust::get<1>(t);
        scalar rhow = thrust::get<2>(t);
        scalar magGradU = thrust::get<3>(t);
        scalar magUp = thrust::get<4>(t);
		
        scalar ut =
            sqrt((mutw + muw)*magGradU/rhow);

        if (ut > ROOTVSMALL)
        {
            int iter = 0;
            scalar err = GREAT;

            do
            {
                scalar kUu = min(kappa*magUp/ut, 50);
                scalar fkUu = exp(kUu) - 1 - kUu*(1 + 0.5*kUu);

                scalar f =
                    - ut*y/(muw/rhow)
                    + magUp/ut
                    + 1/E*(fkUu - 1.0/6.0*kUu*sqr(kUu));

                scalar df =
                    y/(muw/rhow)
                  + magUp/sqr(ut)
                  + 1/E*kUu*fkUu/ut;

                scalar uTauNew = ut + f/df;
                err = mag((ut - uTauNew)/ut);
                ut = uTauNew;

            } while (ut > ROOTVSMALL && err > 0.01 && ++iter < 10);

            return max(0.0, ut);
        }
        else
        {
			return 0;
        }
    }
};

// * * * * * * * * * * * * Protected Member Functions  * * * * * * * * * * * //

tmp<scalargpuField> mutUSpaldingWallFunctionFvPatchScalarField::calcUTau
(
    const scalargpuField& magGradU
) const
{
    const turbulenceModel& turbModel =
        db().lookupObject<turbulenceModel>("turbulenceModel");

    const scalargpuField& y = turbModel.y()[patch().index()];

    const fvPatchVectorField& Uw =
        turbModel.U().boundaryField()[patch().index()];

    scalargpuField magUp(mag(Uw.patchInternalField() - Uw));

    const fvPatchScalarField& rhow =
        turbModel.rho().boundaryField()[patch().index()];

    const fvPatchScalarField& muw =
        turbModel.mu().boundaryField()[patch().index()];
    const scalargpuField& mutw = *this;

    tmp<scalargpuField> tuTau(new scalargpuField(patch().size(), 0.0));
    scalargpuField& uTau = tuTau();
    
    thrust::transform
    (
        y.begin(),
        y.end(),
        thrust::make_zip_iterator(thrust::make_tuple
        (
            muw.begin(),
            mutw.begin(),
            rhow.begin(),
            magGradU.begin(),
            magUp.begin()
        )),
        uTau.begin(),
        mutUSpaldingCalcUTauFunctor(kappa_,E_)
    );

/*
    forAll(mutw, faceI)
    {
        scalar ut =
            sqrt((mutw[faceI] + muw[faceI])*magGradU[faceI]/rhow[faceI]);

        if (ut > ROOTVSMALL)
        {
            int iter = 0;
            scalar err = GREAT;

            do
            {
                scalar kUu = min(kappa_*magUp[faceI]/ut, 50);
                scalar fkUu = exp(kUu) - 1 - kUu*(1 + 0.5*kUu);

                scalar f =
                    - ut*y[faceI]/(muw[faceI]/rhow[faceI])
                    + magUp[faceI]/ut
                    + 1/E_*(fkUu - 1.0/6.0*kUu*sqr(kUu));

                scalar df =
                    y[faceI]/(muw[faceI]/rhow[faceI])
                  + magUp[faceI]/sqr(ut)
                  + 1/E_*kUu*fkUu/ut;

                scalar uTauNew = ut + f/df;
                err = mag((ut - uTauNew)/ut);
                ut = uTauNew;

            } while (ut > ROOTVSMALL && err > 0.01 && ++iter < 10);

            uTau[faceI] = max(0.0, ut);
        }
    }
*/
    return tuTau;
}


tmp<scalargpuField> mutUSpaldingWallFunctionFvPatchScalarField::calcMut() const
{
    const label patchi = patch().index();

    const turbulenceModel& turbModel =
        db().lookupObject<turbulenceModel>("turbulenceModel");

    const fvPatchVectorField& Uw = turbModel.U().boundaryField()[patchi];
    const scalargpuField magGradU(mag(Uw.snGrad()));
    const scalargpuField& rhow = turbModel.rho().boundaryField()[patchi];
    const scalargpuField& muw = turbModel.mu().boundaryField()[patchi];

    return max
    (
        scalar(0),
        rhow*sqr(calcUTau(magGradU))/(magGradU + ROOTVSMALL) - muw
    );
}


// * * * * * * * * * * * * * * * * Constructors  * * * * * * * * * * * * * * //

mutUSpaldingWallFunctionFvPatchScalarField::
mutUSpaldingWallFunctionFvPatchScalarField
(
    const fvPatch& p,
    const DimensionedField<scalar, volMesh>& iF
)
:
    mutWallFunctionFvPatchScalarField(p, iF)
{}


mutUSpaldingWallFunctionFvPatchScalarField::
mutUSpaldingWallFunctionFvPatchScalarField
(
    const mutUSpaldingWallFunctionFvPatchScalarField& ptf,
    const fvPatch& p,
    const DimensionedField<scalar, volMesh>& iF,
    const fvPatchFieldMapper& mapper
)
:
    mutWallFunctionFvPatchScalarField(ptf, p, iF, mapper)
{}


mutUSpaldingWallFunctionFvPatchScalarField::
mutUSpaldingWallFunctionFvPatchScalarField
(
    const fvPatch& p,
    const DimensionedField<scalar, volMesh>& iF,
    const dictionary& dict
)
:
    mutWallFunctionFvPatchScalarField(p, iF, dict)
{}


mutUSpaldingWallFunctionFvPatchScalarField::
mutUSpaldingWallFunctionFvPatchScalarField
(
    const mutUSpaldingWallFunctionFvPatchScalarField& wfpsf
)
:
    mutWallFunctionFvPatchScalarField(wfpsf)
{}


mutUSpaldingWallFunctionFvPatchScalarField::
mutUSpaldingWallFunctionFvPatchScalarField
(
    const mutUSpaldingWallFunctionFvPatchScalarField& wfpsf,
    const DimensionedField<scalar, volMesh>& iF
)
:
    mutWallFunctionFvPatchScalarField(wfpsf, iF)
{}


// * * * * * * * * * * * * * * * Member Functions  * * * * * * * * * * * * * //

tmp<scalargpuField> mutUSpaldingWallFunctionFvPatchScalarField::yPlus() const
{
    const label patchi = patch().index();

    const turbulenceModel& turbModel =
        db().lookupObject<turbulenceModel>("turbulenceModel");

    const scalargpuField& y = turbModel.y()[patchi];
    const fvPatchVectorField& Uw = turbModel.U().boundaryField()[patchi];
    const scalargpuField& rhow = turbModel.rho().boundaryField()[patchi];
    const scalargpuField& muw = turbModel.mu().boundaryField()[patchi];

    return y*calcUTau(mag(Uw.snGrad()))/(muw/rhow);
}


void mutUSpaldingWallFunctionFvPatchScalarField::write(Ostream& os) const
{
    fvPatchField<scalar>::write(os);
    writeLocalEntries(os);
    writeEntry("value", os);
}


// * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * //

makePatchTypeField
(
    fvPatchScalarField,
    mutUSpaldingWallFunctionFvPatchScalarField
);

// * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * //

} // End namespace compressible
} // End namespace Foam

// ************************************************************************* //
