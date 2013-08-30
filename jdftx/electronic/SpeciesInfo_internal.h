/*-------------------------------------------------------------------
Copyright 2012 Ravishankar Sundararaman

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

#ifndef JDFTX_ELECTRONIC_SPECIESINFO_INTERNAL_H
#define JDFTX_ELECTRONIC_SPECIESINFO_INTERNAL_H

//!@file SpeciesInfo_internal.h Shared GPU/CPU code for ion/pseudopotential related calculations

#include <core/matrix3.h>
#include <electronic/RadialFunction.h>
#include <electronic/SphericalHarmonics.h>

//! Compute Vnl and optionally its gradients for a subset of the basis space, and for multiple atomic positions
template<int l, int m> __hostanddev__
void Vnl_calc(int n, int atomStride, int nAtoms, const vector3<>& k, const vector3<int>* iGarr, const matrix3<>& G,
	const vector3<>* pos, const RadialFunctionG& VnlRadial, complex* Vnl, bool computeGrad, vector3<complex*> dV)
{
	vector3<> kpG = k + iGarr[n]; //k+G in reciprocal lattice coordinates:
	vector3<> qvec = kpG * G; //k+G in cartesian coordinates
	double q = qvec.length();
	vector3<> qhat = qvec * (q ? 1.0/q : 0.0); //the unit vector along qvec (set qhat to 0 for q=0 (doesn't matter))
	//Compute the prefactor to the sturtcure factor in Vnl:
	double prefac = Ylm<l,m>(qhat) * VnlRadial(q);
	//Loop over columns (multiple atoms at same l,m):
	for(int atom=0; atom<nAtoms; atom++)
	{	//Multiply above prefactor by the structure factor for current atom
		complex temp = prefac * cis((-2*M_PI)*dot(pos[atom],kpG));
		Vnl[atom*atomStride+n] = temp;
		//Also set the gradients if requested
		if(computeGrad)
		{	temp *= complex(0,-2*M_PI);
			storeVector(temp*kpG, dV, atom*atomStride+n);
		}
	}
}
void Vnl(int nbasis, int atomStride, int nAtoms, int l, int m, const vector3<> k, const vector3<int>* iGarr, const matrix3<> G,
	const vector3<>* pos, const RadialFunctionG& VnlRadial, complex* Vnl, bool computeGrad, vector3<complex*> dV);
#ifdef GPU_ENABLED
void Vnl_gpu(int nbasis, int atomStride, int nAtoms, int l, int m, const vector3<> k, const vector3<int>* iGarr, const matrix3<> G,
	const vector3<>* pos, const RadialFunctionG& VnlRadial, complex* Vnl, bool computeGrad, vector3<complex*> dV);
#endif


//! Perform the loop:
//!   for(lm=0; lm < Nlm; lm++) (*f)(tag< lm >);
//! at compile time using templates. Note with lm := l*(l+1)+m to loop over spherical harmonics, Nlm = (lMax+1)^2.
//! The functor f is templated over lm by using the tag class StaticLoopYlmTag< lm >.
template<int lm> struct StaticLoopYlmTag{};
template<int Nlm, typename Functor, int lmInv> struct StaticLoopYlm
{	__hostanddev__ static void exec(Functor* f)
	{	(*f)(StaticLoopYlmTag<Nlm-lmInv>());
		StaticLoopYlm< Nlm, Functor, lmInv-1 >::exec(f);
	}
};
template<int Nlm, typename Functor> struct StaticLoopYlm< Nlm, Functor, 0>
{	__hostanddev__ static void exec(Functor* f) { return; }
};
template<int Nlm, typename Functor> __hostanddev__ void staticLoopYlm(Functor* f)
{	StaticLoopYlm< Nlm, Functor, Nlm >::exec(f);
}

#define SwitchTemplate_Nlm(Nlm, func, args) \
	switch(Nlm) \
	{	case 1:  func<1>  args; break; \
		case 4:  func<4>  args; break; \
		case 9:  func<9>  args; break; \
		case 16: func<16> args; break; \
		case 25: func<25> args; break; \
		case 49: func<49> args; break; \
		default: fprintf(stderr, "Invalid Nlm in SwitchTemplate_Nlm"); exit(1); \
	}


//! Augment electron density by spherical functions (radial functions multiplied by spherical harmonics)
//! and propagate gradient w.r.t to it to that w.r.t the atom position (accumulate)
struct nAugmentFunctor
{	vector3<> qhat; double q;
	int nGloc; double dGinv; const double* nRadial;
	complex n;
	
	__hostanddev__ nAugmentFunctor(const vector3<>& qvec, int nGloc, double dGinv, const double* nRadial)
	: nGloc(nGloc), dGinv(dGinv), nRadial(nRadial)
	{	q = qvec.length();
		qhat = qvec * (q ? 1.0/q : 0.0); //the unit vector along qvec (set qhat to 0 for q=0 (doesn't matter))
	}
	
	template<int lm> __hostanddev__ void operator()(const StaticLoopYlmTag<lm>&)
	{	//Compute phase (-i)^l:
		complex mIota(0,-1), phase(1,0);
		for(int l=0; l*(l+2) < lm; l++) phase *= mIota;
		//Accumulate result:
		double Gindex = q * dGinv;
		if(Gindex < nGloc-5)
			n += phase * Ylm<lm>(qhat) * QuinticSpline::value(nRadial+lm*nGloc, Gindex); 
	}
};
template<int Nlm> __hostanddev__
void nAugment_calc(int i, const vector3<int>& iG, const matrix3<>& G,
	int nGloc, double dGinv, const double* nRadial, const vector3<>& atpos, complex* n)
{
	nAugmentFunctor functor(iG*G, nGloc, dGinv, nRadial);
	staticLoopYlm<Nlm>(&functor);
	n[i] += functor.n * cis((-2*M_PI)*dot(atpos,iG));
}
void nAugment(int Nlm,
	const vector3<int> S, const matrix3<>& G,
	int nGloc, double dGinv, const double* nRadial, const vector3<>& atpos, complex* n);
#ifdef GPU_ENABLED
void nAugment_gpu(int Nlm,
	const vector3<int> S, const matrix3<>& G,
	int nGloc, double dGinv, const double* nRadial, const vector3<>& atpos, complex* n);
#endif

//Gradient propragation corresponding to nAugment:
struct nAugmentGradFunctor
{	vector3<> qhat; double q;
	int nGloc; double dGinv; const double* nRadial;
	complex E_n, nE_n;
	double* E_nRadial;
	int dotPrefac; //prefactor in dot-product (1 or 2 for each reciprocal space point, because of real symmetry)
	
	__hostanddev__ nAugmentGradFunctor(const vector3<>& qvec, int nGloc, double dGinv, const double* nRadial, const complex& E_n, double* E_nRadial, int dotPrefac)
	: nGloc(nGloc), dGinv(dGinv), nRadial(nRadial), E_n(E_n), E_nRadial(E_nRadial), dotPrefac(dotPrefac)
	{	q = qvec.length();
		qhat = qvec * (q ? 1.0/q : 0.0); //the unit vector along qvec (set qhat to 0 for q=0 (doesn't matter))
	}
	
	template<int lm> __hostanddev__ void operator()(const StaticLoopYlmTag<lm>&)
	{	//Compute phase (-i)^l:
		complex mIota(0,-1), phase(1,0);
		for(int l=0; l*(l+2) < lm; l++) phase *= mIota;
		//Accumulate result:
		double Gindex = q * dGinv;
		if(Gindex < nGloc-5)
		{	complex term = phase * Ylm<lm>(qhat) * E_n;
			QuinticSpline::valueGrad(dotPrefac * term.real(), E_nRadial+lm*nGloc, Gindex);
			if(nRadial) nE_n += term * QuinticSpline::value(nRadial+lm*nGloc, Gindex); //needed again only when computing forces
		}
	}
};
template<int Nlm> __hostanddev__
void nAugmentGrad_calc(int i, const vector3<int>& iG, const matrix3<>& G,
	int nGloc, double dGinv, const double* nRadial, const vector3<>& atpos,
	const complex* ccE_n, double* E_nRadial, vector3<complex*> E_atpos, int dotPrefac)
{
	nAugmentGradFunctor functor(iG*G, nGloc, dGinv, nRadial, ccE_n[i].conj() * cis((-2*M_PI)*dot(atpos,iG)), E_nRadial, dotPrefac);
	staticLoopYlm<Nlm>(&functor);
	if(nRadial) accumVector((functor.nE_n * complex(0,-2*M_PI)) * iG, E_atpos, i);
}
void nAugmentGrad(int Nlm, const vector3<int> S, const matrix3<>& G,
	int nGloc, double dGinv, const double* nRadial, const vector3<>& atpos,
	const complex* ccE_n, double* E_nRadial, vector3<complex*> E_atpos);
// #ifdef GPU_ENABLED
//  TODO: Not trivial to GPU parallelize because of overlapping scattered writes to E_nRadial 
// #endif


//!Get structure factor for a specific iG, given a list of atoms
__hostanddev__ complex getSG_calc(const vector3<int>& iG, const int& nAtoms, const vector3<>* atpos)
{	complex SG = complex(0,0);
	for(int atom=0; atom<nAtoms; atom++)
		SG += cis(-2*M_PI*dot(iG,atpos[atom]));
	return SG;
}
//!Get structure factor in a DataGptr's data/dataGpu (with 1/vol normalization factor)
void getSG(const vector3<int> S, int nAtoms, const vector3<>* atpos, double invVol, complex* SG);
#ifdef GPU_ENABLED
void getSG_gpu(const vector3<int> S, int nAtoms, const vector3<>* atpos, double invVol, complex* SG);
#endif

//! Calculate local pseudopotential, ionic density and chargeball due to one species at a given G-vector
__hostanddev__ void updateLocal_calc(int i, const vector3<int>& iG, const matrix3<>& GGT,
	complex *Vlocps, complex *rhoIon, complex *nChargeball, complex* nCore, complex* tauCore,
	int nAtoms, const vector3<>* atpos, double invVol, const RadialFunctionG& VlocRadial,
	double Z, const RadialFunctionG& nCoreRadial, const RadialFunctionG& tauCoreRadial,
	double Zchargeball, double wChargeball)
{
	double Gsq = GGT.metric_length_squared(iG);

	//Compute structure factor (scaled by 1/detR):
	complex SGinvVol = getSG_calc(iG, nAtoms, atpos) * invVol;

	//Short-ranged part of Local potential (long-ranged part added on later in IonInfo.cpp):
	Vlocps[i] += SGinvVol * VlocRadial(sqrt(Gsq));

	//Nuclear charge (optionally widened to a gaussian later in IonInfo.cpp):
	rhoIon[i] += SGinvVol * (-Z);

	//Chargeball:
	if(nChargeball)
		nChargeball[i] += SGinvVol * Zchargeball
			* pow(sqrt(2*M_PI)*wChargeball,3)*exp(-0.5*Gsq*pow(wChargeball,2));

	//Partial core:
	if(nCore) nCore[i] += SGinvVol * nCoreRadial(sqrt(Gsq));
	if(tauCore) tauCore[i] += SGinvVol * tauCoreRadial(sqrt(Gsq));
}
void updateLocal(const vector3<int> S, const matrix3<> GGT,
	complex *Vlocps,  complex *rhoIon, complex *n_chargeball, complex* n_core, complex* tauCore,
	int nAtoms, const vector3<>* atpos, double invVol, const RadialFunctionG& VlocRadial,
	double Z, const RadialFunctionG& nCoreRadial, const RadialFunctionG& tauCoreRadial,
	double Zchargeball, double wChargeball);
#ifdef GPU_ENABLED
void updateLocal_gpu(const vector3<int> S, const matrix3<> GGT,
	complex *Vlocps,  complex *rhoIon, complex *n_chargeball, complex* n_core, complex* tauCore,
	int nAtoms, const vector3<>* atpos, double invVol, const RadialFunctionG& VlocRadial,
	double Z, const RadialFunctionG& nCoreRadial, const RadialFunctionG& tauCoreRadial,
	double Zchargeball, double wChargeball);
#endif


//! Propagate (complex conjugates of) gradients w.r.t Vlocps, rhoIon etc to complex conjugate gradient w.r.t SG (the strutcure factor)
__hostanddev__ void gradLocalToSG_calc(int i, const vector3<int> iG, const matrix3<> GGT,
	const complex* ccgrad_Vlocps, const complex* ccgrad_rhoIon, const complex* ccgrad_nChargeball,
	const complex* ccgrad_nCore, const complex* ccgrad_tauCore, complex* ccgrad_SG,
	const RadialFunctionG& VlocRadial, double Z,
	const RadialFunctionG& nCoreRadial, const RadialFunctionG& tauCoreRadial,
	double Zchargeball, double wChargeball)
{
	double Gsq = GGT.metric_length_squared(iG);
	complex ccgrad_SGinvVol(0,0); //result for this G value (gradient w.r.t structure factor/volume)

	//Local potential (short ranged part in the radial function - Z/r):
	ccgrad_SGinvVol += ccgrad_Vlocps[i] * VlocRadial(sqrt(Gsq));

	//Nuclear charge
	if(ccgrad_rhoIon)
		ccgrad_SGinvVol += ccgrad_rhoIon[i] * (-Z);

	//Chargeball:
	if(ccgrad_nChargeball)
		ccgrad_SGinvVol += ccgrad_nChargeball[i] * Zchargeball
			* pow(sqrt(2*M_PI)*wChargeball,3)*exp(-0.5*Gsq*pow(wChargeball,2));

	//Partial core:
	if(ccgrad_nCore) ccgrad_SGinvVol += ccgrad_nCore[i] * nCoreRadial(sqrt(Gsq));
	if(ccgrad_tauCore) ccgrad_SGinvVol += ccgrad_tauCore[i] * tauCoreRadial(sqrt(Gsq));
	
	//Store result:
	ccgrad_SG[i] = ccgrad_SGinvVol;
}
void gradLocalToSG(const vector3<int> S, const matrix3<> GGT,
	const complex* ccgrad_Vlocps, const complex* ccgrad_rhoIon, const complex* ccgrad_nChargeball,
	const complex* ccgrad_nCore, const complex* ccgrad_tauCore, complex* ccgrad_SG,
	const RadialFunctionG& VlocRadial, double Z,
	const RadialFunctionG& nCoreRadial, const RadialFunctionG& tauCoreRadial,
	double Zchargeball, double wChargeball);
#ifdef GPU_ENABLED
void gradLocalToSG_gpu(const vector3<int> S, const matrix3<> GGT,
	const complex* ccgrad_Vlocps, const complex* ccgrad_rhoIon, const complex* ccgrad_nChargeball,
	const complex* ccgrad_nCore, const complex* ccgrad_tauCore, complex* ccgrad_SG,
	const RadialFunctionG& VlocRadial, double Z,
	const RadialFunctionG& nCoreRadial, const RadialFunctionG& tauCoreRadial,
	double Zchargeball, double wChargeball);
#endif


//! Propagate the complex conjugate gradient w.r.t the structure factor to the given atomic position
//! this is still per G-vector, need to sum grad_atpos over G to get the force on that atom
__hostanddev__ void gradSGtoAtpos_calc(int i, const vector3<int> iG, const vector3<> atpos,
	const complex* ccgrad_SG, vector3<complex*> grad_atpos)
{
	complex term = complex(0,-2*M_PI) * cis(-2*M_PI*dot(iG,atpos)) * ccgrad_SG[i].conj();
	storeVector(iG * term, grad_atpos, i);
}
void gradSGtoAtpos(const vector3<int> S, const vector3<> atpos,
	const complex* ccgrad_SG, vector3<complex*> grad_atpos);
#ifdef GPU_ENABLED
void gradSGtoAtpos_gpu(const vector3<int> S, const vector3<> atpos,
	const complex* ccgrad_SG, vector3<complex*> grad_atpos);
#endif


#endif // JDFTX_ELECTRONIC_SPECIESINFO_INTERNAL_H
