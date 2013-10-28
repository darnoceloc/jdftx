/*-------------------------------------------------------------------
Copyright 2011 Ravishankar Sundararaman

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

#ifndef JDFTX_CORE_GRIDINFO_H
#define JDFTX_CORE_GRIDINFO_H

/** @file GridInfo.h
@brief Geometry of the simulation grid
*/

#include <core/matrix3.h>
#include <core/Data.h>
#include <core/GpuUtil.h>
#include <map>
#include <fftw3.h>
#include <stdint.h>
#include <cstdio>

/** @brief Simulation grid descriptor
//! @ingroup griddata

To setup a simulation grid, create a blank GridInfo object,
set the public data members #S and #R, and call #initialize().

This sets up all the auxilliary grid information, and  a bunch
of shared utilities such as a random number generator,
plans for Fourier transforms etc.
*/
class GridInfo
{
public:

	GridInfo();
	~GridInfo();

	void update(); //! Update the grid information on changing the lattice vectors
	void printLattice(); //! Print the lattice vectors
	void printReciprocalLattice(); //! Print the reciprocal lattice vectors
	
	enum LatticeType
	{	Manual, //!< Directly specify R
		//7 lattice systems specified using sides a,b,c and alpha,beta,gamma
		Triclinic,
		Monoclinic,
		Orthorhombic,
		Tetragonal,
		Rhombohedral,
		Hexagonal,
		Cubic
	}
	latticeType;
	
	enum LatticeModification
	{	Simple,
		BodyCentered,
		BaseCentered,
		FaceCentered
	}
	latticeModification;
	
	double a, b, c, alpha, beta, gamma; //!< lattice specified by type, modification and standard side lengths, angles in degrees
	void setLatticeVectors(); //!< Set lattice vectors based on lattice type, modification and parameters above
	
	vector3<> lattScale; //!< Remember lattice scale specified at input (Note R always includes scale, once latt-scale has processed)
	
	matrix3<> R; //!< directly specified lattice vectors
	double Gmax; //!< radius of wavefunction G-sphere, whole density sphere (double the radius) must be inscribable within the FFT box
	double GmaxRho; //!< if non-zero, override the FFT box inscribable sphere radius
	vector3<int> S; //!< sample points in each dimension (if 0, will be determined automatically based on Gmax)

	//! Initialize the dependent quantities below.
	//! If S is specified and is too small for the given Gmax, the call will abort.
	//! If skipHeader=true, the "initializing the grid" banner will be supressed (useful for auxiliary grids in calculation)
	//! If sym is specified, then auto-computed S will be ensured commensurate with those symmetries
	void initialize(bool skipHeader=false, const std::vector< matrix3<int> > sym = std::vector< matrix3<int> >(1, matrix3<int>(1,1,1)));

	double detR; //!< cell volume
	matrix3<> RT, RTR, invR, invRT, invRTR; //!< various combinations of lattice-vectors
	matrix3<> G, GT, GGT, invGGT; //!< various combinations of reciporcal lattice-vectors

	double dV; //!< volume per grid point
	vector3<> h[3]; //!< real space sample vectors
	int nr; //!< position space grid count = S[0]*S[1]*S[2]
	int nG; //!< reciprocal lattice count = S[0]*S[1]*(S[2]/2+1) [on account of using r2c and c2r ffts]

	double dGradial; //!< recommended spacing of radial G functions
	double GmaxSphere; //!< recommended maximum G-vector for radial functions for the wavefunction sphere
	double GmaxGrid; //!< recommended maximum G-vector for radial functions for the density grid

	//FFT plans:
	fftw_plan planForwardSingle; //!< Single-thread Forward complex transform
	fftw_plan planInverseSingle; //!< Single-thread Inverse complex transform
	fftw_plan planForwardInPlaceSingle; //!< Single-thread Forward in-place complex transform
	fftw_plan planInverseInPlaceSingle; //!< Single-thread Inverse in-place complex transform
	fftw_plan planRtoCsingle; //!< Single-thread FFTW plan for R -> G
	fftw_plan planCtoRsingle; //!< Single-thread FFTW plan for G -> R
	fftw_plan planForwardMulti; //!< Multi-threaded Forward complex transform
	fftw_plan planInverseMulti; //!< Multi-threaded Inverse complex transform
	fftw_plan planForwardInPlaceMulti; //!< Multi-threaded Forward in-place complex transform
	fftw_plan planInverseInPlaceMulti; //!< Multi-threaded Inverse in-place complex transform
	fftw_plan planRtoCmulti; //!< Multi-threaded FFTW plan for R -> G
	fftw_plan planCtoRmulti; //!< Multi-threaded FFTW plan for G -> R
	#ifdef GPU_ENABLED
	cufftHandle planZ2Z; //!< CUFFT plan for all the complex transforms
	cufftHandle planD2Z; //!< CUFFT plan for R -> G
	cufftHandle planZ2D; //!< CUFFT plan for G -> R
	cufftHandle planZ2Dcompat; //!< CUFFT plan for G -> R in FFTW compatibility mode (required when nyquist component is assymetric)
	#endif

	//Indexing utilities (inlined for efficiency)
	inline vector3<int> wrapGcoords(const vector3<int> iG) const //!< wrap negative G-indices to the positive side
	{	vector3<int> iGwrapped = iG;
		for(int k=0; k<3; k++)
			if(iGwrapped[k]<0)
				iGwrapped[k] += S[k];
		return iGwrapped;
	}
	inline int fullRindex(const vector3<int> iR) const //!< Index into the full real-space box:
	{	return iR[2] + S[2]*(iR[1] + S[1]*iR[0]);
	}
	inline int fullGindex(const vector3<int> iG) const //!< Index into the full reciprocal-space box:
	{	return fullRindex(wrapGcoords(iG));
	}
	inline int halfGindex(const vector3<int> iG) const //!< Index into the half-reduced reciprocal-space box:
	{	vector3<int> iGwrapped = wrapGcoords(iG);
		return iGwrapped[2] + (S[2]/2+1)*(iGwrapped[1] + S[1]*iGwrapped[0]);
	}

private:
	bool initialized; //!< keep track of whether initialize() has been called
	void updateSdependent();
};

#endif //JDFTX_CORE_GRIDINFO_H
