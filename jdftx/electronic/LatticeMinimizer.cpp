/*-------------------------------------------------------------------
Copyright 2012 Deniz Gunceler, Ravishankar Sundararaman

This file is part of JDFTx.

JDFTx is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

JDFTx is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with JDFTx.  If not, see <http://www.gnu.org/licenses/>.
-------------------------------------------------------------------*/

#include <electronic/LatticeMinimizer.h>
#include <electronic/Everything.h>
#include <core/LatticeUtils.h>
#include <core/Random.h>

//Functions required by Minimizable<matrix3<>>
void axpy(double alpha, const matrix3<>& x, matrix3<>& y) { y += alpha * x; }
double dot(const matrix3<>& x, const matrix3<>& y) { return trace(x*(~y)); }
matrix3<> clone(const matrix3<>& x) { return x; }
void randomize(matrix3<>& x) { for(int i=0; i<3; i++) for(int j=0; j<3; j++) x(i,j) = Random::normal(); }


//-------------  class LatticeMinimizer -----------------

LatticeMinimizer::LatticeMinimizer(Everything& e) : e(e), Rorig(e.gInfo.R)
{
	logPrintf("\n--------- Lattice Minimization ---------\n");
	
	//Ensure that lattice-move-scale is commensurate with symmetries:
	std::vector<matrix3<int>> sym = e.symm.getMatrices();
	for(const matrix3<int>& m: sym)
		for(int i=0; i<3; i++)
			for(int j=0; j<3; j++)
				if(m(i,j) && e.cntrl.lattMoveScale[i] != e.cntrl.lattMoveScale[j])
					die("latt-move-scale is not commensurate with symmetries:\n"
						"\t(Lattice vectors #%d and #%d are connected by symmetry,\n"
						"\tbut have different move scale factors %lg != %lg).\n",
						i, j, e.cntrl.lattMoveScale[i], e.cntrl.lattMoveScale[j]);
	
	//Check which lattice vectors can be altered:
	vector3<bool> isFixed, isTruncated = e.coulombParams.isTruncated();
	for(int k=0; k<3; k++)
		isFixed[k] = (e.cntrl.lattMoveScale[k]==0.) || isTruncated[k];
	
	//Create a orthonormal basis for strain commensurate with symmetries:
	for(int k=0; k<6; k++)
	{	//Initialize a basis element for arbitrary symmetric matrices:
		matrix3<int> s; //all zero:
		if(k<3) //diagonal strain
		{	s(k,k) = 1;
			if(isFixed[k]) continue; //strain alters fixed direction
		}
		else //off-diagonal strain
		{	int i=(k+1)%3;
			int j=(k+2)%3;
			s(i,j) = s(j,i) = 1;
			if(isFixed[i] || isFixed[j]) continue;  //strain alters fixed direction
		}
		//Symmetrize:
		matrix3<int> sSym;
		for(const matrix3<int>& m: sym)
		{	matrix3<int> mInv = det(m) * adjugate(m); //since |det(m)| = 1
			sSym += mInv * s * m;
		}
		//Orthonormalize w.r.t previous basis elements:
		matrix3<> strain(sSym); //convert from integer to double matrix
		for(const matrix3<>& sPrev: strainBasis)
			strain -= sPrev * dot(sPrev, strain);
		double strainNorm = nrm2(strain);
		if(strainNorm < symmThresholdSq) continue; //linearly dependent
		strainBasis.push_back((1./strainNorm) * strain);
	}
	if(!strainBasis.size())
		die("All lattice-vectors are constrained by coulomb truncation and/or\n"
			"latt-move-scale: please disable lattice minimization.\n");
	
	//Print initialization status:
	e.latticeMinParams.nDim = strainBasis.size();
	logPrintf("Minimization of dimension %lu over strains spanned by:\n", strainBasis.size());
	for(const matrix3<>& s: strainBasis)
	{	s.print(globalLog, " %lg ");
		logPrintf("\n");
	}

	h = 1e-5;
}

void LatticeMinimizer::step(const matrix3<>& dir, double alpha)
{	strain += alpha * dir;
}

double LatticeMinimizer::compute(matrix3<>* grad)
{
	//Check for large lattice strain
	if(sqrt(dot(strain, strain)) > maxAllowedStrain)
	{	logPrintf("\nBacking of lattice step since strain tensor has become enormous:\n"); strain.print(globalLog, "%10lg ");
		logPrintf("If this is a physical strain, restart calculation with these lattice vectors to prevent Pulay errors:\n");
		e.gInfo.printLattice();
		logPrintf("\n");
		return NAN;
	}

	e.gInfo.R = Rorig + Rorig*strain; // Updates the lattice vectors to current strain
	updateLatticeDependent(true); // Updates lattice information and gets the energy (but does not touch electronic state, important for magnetic LCAO)
	
	//! Run an ionic minimizer at the current strain
	IonicMinimizer ionicMinimizer(e);
	ionicMinimizer.minimize(e.ionicMinParams);
	
	//! If asked for, returns the gradient of the strain tensor
	if(grad)
	{	//! Loop over all basis vectors and get the gradient.
		*grad = matrix3<>();
		auto stress = calculateStress();
		for(size_t i=0; i<strainBasis.size(); i++)
			*grad += stress[i]*strainBasis[i];
		e.gInfo.R = Rorig + Rorig*strain;
		updateLatticeDependent();
	}
	
	return relevantFreeEnergy(e);
}

std::vector< double > LatticeMinimizer::calculateStress()
{
	std::vector<double> stress(strainBasis.size());
	
	logPrintf("\nCalculating stress tensor... "); logFlush();
	for(size_t i=0; i<strainBasis.size(); i++)
		stress[i] = centralDifference(strainBasis[i]);
	logPrintf(" done!\n");
	
	return stress;
}

double LatticeMinimizer::centralDifference(matrix3<> direction)
{ //! Implements a central difference derivative with O(h^4)
	
	e.gInfo.R = Rorig + Rorig*(strain+(-2*h*direction));
	updateLatticeDependent();
	const double En2h = relevantFreeEnergy(e);
	
	e.gInfo.R = Rorig + Rorig*(strain+(-h*direction));
	updateLatticeDependent();
	const double Enh = relevantFreeEnergy(e);
	
	e.gInfo.R = Rorig + Rorig*(strain+(h*direction));
	updateLatticeDependent();
	const double Eph = relevantFreeEnergy(e);
	
	e.gInfo.R = Rorig + Rorig*(strain+(2*h*direction));
	updateLatticeDependent();
	const double Ep2h = relevantFreeEnergy(e);
	
	return (1./(12.*h))*(En2h - 8.*Enh + 8.*Eph - Ep2h);
}

matrix3<> LatticeMinimizer::precondition(const matrix3<>& grad)
{	return Diag(e.cntrl.lattMoveScale) * grad * Diag(e.cntrl.lattMoveScale);
}

bool LatticeMinimizer::report(int iter)
{	logPrintf("\n");
	e.gInfo.printLattice();
	e.gInfo.printReciprocalLattice();
	logPrintf("\nStrain Tensor = \n"); strain.print(globalLog, "%10lg ");
	logPrintf("\n");
	e.dump(DumpFreq_Lattice, iter);
	return false;
}

void LatticeMinimizer::constrain(matrix3<>& dir)
{	matrix3<> result;
	for(const matrix3<>& s: strainBasis)
		result += s * dot(s, dir); //projection in basis
	dir = result;
}

void LatticeMinimizer::updateLatticeDependent(bool ignoreElectronic)
{	logSuspend();
	e.gInfo.update();
	e.updateSupercell();
	e.coulomb = e.coulombParams.createCoulomb(e.gInfo);
	e.iInfo.update(e.ener);
	if(!ignoreElectronic) e.eVars.elecEnergyAndGrad(e.ener);
	logResume();
}

void LatticeMinimizer::restore()
{	strain *= 0;
	e.gInfo.R = Rorig;
	updateLatticeDependent();
}
