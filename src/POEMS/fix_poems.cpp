/* ----------------------------------------------------------------------
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   http://lammps.sandia.gov, Sandia National Laboratories
   Steve Plimpton, sjplimp@sandia.gov

   Copyright (2003) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under 
   the GNU General Public License.

   See the README file in the top-level LAMMPS directory.
------------------------------------------------------------------------- */

/* ----------------------------------------------------------------------
   FixPOEMS is a LAMMPS interface to the POEMS coupled multi-body simulator
   POEMS authors: Rudranarayan Mukherjee (mukher@rpi.edu)
                  Kurt Anderson (anderk5@rpi.edu)
------------------------------------------------------------------------- */

#include "mpi.h"
#include "math.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include "workspace.h"
#include "fix_poems.h"
#include "atom.h"
#include "domain.h"
#include "update.h"
#include "respa.h"
#include "modify.h"
#include "force.h"
#include "output.h"
#include "group.h"
#include "comm.h"
#include "memory.h"
#include "error.h"

#include "fix_property_atom.h"

using namespace LAMMPS_NS;
using namespace FixConst;

#define MAXBODY 2         // currently 2 since only linear chains allowed
#define DELTA 128
#define TOLERANCE 1.0e-1
#define EPSILON 1.0e-7
#define MAXJACOBI 50

/* ----------------------------------------------------------------------
   define rigid bodies and joints, initiate POEMS
------------------------------------------------------------------------- */

FixPOEMS::FixPOEMS(LAMMPS *lmp, int narg, char **arg) :
  Fix(lmp, narg, arg)
{
  mydebug=0;
  int i,j,ibody;

  time_integrate = 1;
  rigid_flag = 1;
  virial_flag = 1;

  MPI_Comm_rank(world,&me);

  // perform initial allocation of atom-based arrays
  // register with atom class

  natom2body = NULL;
  atom2body = NULL;
  displace = NULL;
  grow_arrays(atom->nmax);
  atom->add_callback(0);

  // initialize each atom to belong to no rigid bodies

  int nlocal = atom->nlocal;
  for (int i = 0; i < nlocal; i++) natom2body[i] = 0;

  // create an atom map if one doesn't exist already
  // readfile() and jointbuild() use global atom IDs

  int mapflag = 0;
  if (atom->map_style == 0) {
    mapflag = 1;
    atom->map_style = 1;
    atom->map_init();
    atom->map_set();
  }

  // parse command-line args
  // set natom2body, atom2body for all atoms and nbody = # of rigid bodies
  // atoms must also be in fix group to be in a body

  if (narg < 4) error->all(FLERR,"Illegal fix poems command");

  // group = arg has list of groups

  if (strcmp(arg[3],"group") == 0) {
    nbody = narg-4;
    if (nbody <= 0) error->all(FLERR,"Illegal fix poems command");

    int *igroups = new int[nbody];
    for (ibody = 0; ibody < nbody; ibody++) {
      igroups[ibody] = group->find(arg[ibody+4]);
      if (igroups[ibody] == -1) 
	    error->all(FLERR,"Could not find fix poems group ID");
    }

    int *mask = atom->mask;

    for (int i = 0; i < nlocal; i++) 
   {
      if (mask[i] & groupbit)
	for (ibody = 0; ibody < nbody; ibody++)
	  if (mask[i] & group->bitmask[igroups[ibody]]) {
	    if (natom2body[i] < MAXBODY) atom2body[i][natom2body[i]] = ibody;
	    natom2body[i]++;
	  }
   }

   delete [] igroups;
    
  // file = read bodies from file
  // file read doesn't pay attention to fix group,
  //   so after read, reset natom2body = 0 if atom is not in fix group

  } else if (strcmp(arg[3],"file") == 0) {

    readfile(arg[4]);

    int *mask = atom->mask;
    for (int i = 0; i < nlocal; i++)
       if (!(mask[i] & groupbit)) natom2body[i] = 1;

  // each molecule in fix group is a rigid body
  // maxmol = largest molecule #
  // ncount = # of atoms in each molecule (have to sum across procs)
  // nbody = # of non-zero ncount values
  // use nall as incremented ptr to set atom2body[] values for each atom

  } else if (strcmp(arg[3],"molecule") == 0) {
    if (narg != 4) error->all(FLERR,"Illegal fix poems command");
    if (atom->molecular == 0)
      error->all(FLERR,"Must use a molecular atom style with fix poems molecule");

    int *mask = atom->mask;
    int *molecule = atom->molecule;
    int nlocal = atom->nlocal;

    int maxmol = -1;
    for (i = 0; i < nlocal; i++)
      if (mask[i] & groupbit) maxmol = MAX(maxmol,molecule[i]);

    int itmp;
    MPI_Allreduce(&maxmol,&itmp,1,MPI_INT,MPI_MAX,world);
    maxmol = itmp + 1;

    int *ncount = new int[maxmol];
    for (i = 0; i < maxmol; i++) ncount[i] = 0;

    for (i = 0; i < nlocal; i++)
      if (mask[i] & groupbit) ncount[molecule[i]]++;

    int *nall = new int[maxmol];
    MPI_Allreduce(ncount,nall,maxmol,MPI_INT,MPI_SUM,world);

    nbody = 0;
    for (i = 0; i < maxmol; i++)
      if (nall[i]) nall[i] = nbody++;
      else nall[i] = -1;

    for (i = 0; i < nlocal; i++) {
      natom2body[i] = 0;
      if (mask[i] & groupbit) {
	natom2body[i] = 1;
	atom2body[i][0] = nall[molecule[i]];
      }
    }
  
    delete [] ncount;
    delete [] nall;

  } else error->all(FLERR,"Illegal fix poems command");

  // error if no bodies
  // error if any atom in too many bodies

  if (nbody == 0) error->all(FLERR,"No rigid bodies defined");

  int flag = 0;
  for (int i = 0; i < nlocal; i++)
    if (natom2body[i] > MAXBODY) flag = 1;
  int flagall;
  MPI_Allreduce(&flag,&flagall,1,MPI_INT,MPI_SUM,world);
  if (flagall) error->all(FLERR,"Atom in too many rigid bodies - boost MAXBODY");

  //create FixPropertyAtom
  fix_xcm = NULL;
  fix_orientationEx = NULL;
  
  // create all nbody-length arrays
  nrigid = new int[nbody];
  masstotal = new double[nbody];
  memory->create(xcm,nbody,3,"poems:xcm");
  memory->create(vcm,nbody,3,"poems:vcm");
  memory->create(fcm,nbody,3,"poems:fcm");
  memory->create(inertia,nbody,3,"poems:inertia");
  memory->create(ex_space,nbody,3,"poems:ex_space");
  memory->create(ey_space,nbody,3,"poems:ey_space");
  memory->create(ez_space,nbody,3,"poems:ez_space");
  memory->create(angmom,nbody,3,"poems:angmom");
  memory->create(omega,nbody,3,"poems:omega");
  memory->create(torque,nbody,3,"poems:torque");

  memory->create(sum,nbody,6,"poems:sum");
  memory->create(all,nbody,6,"poems:all");
  
  // nrigid[n] = # of atoms in Nth rigid body
  // double count joint atoms as being in multiple bodies
  // error if one or zero atoms
  
  int *ncount = new int[nbody];
  for (ibody = 0; ibody < nbody; ibody++) ncount[ibody] = 0;

  for (i = 0; i < nlocal; i++) {
    for (j = 0; j < natom2body[i]; j++)
    {
       ncount[i]++; //exploit fact that atom=segment
    }
//      fprintf(screen, "me: %d, nlocal: %d i: %d, j: %d, ncount: %d \n",
//                me, nlocal, 
//                i, j, 
//                ncount[i]);
  }

  MPI_Allreduce(ncount,nrigid,nbody,MPI_INT,MPI_SUM,world);
  delete [] ncount;


  for (ibody = 0; ibody < nbody; ibody++)
//FIXME: Allow 1 atom in each body - Previously atoms needed to be part of multiple bodies to define a joint
//    if (nrigid[ibody] <= 1) error->all(FLERR,"One or zero atoms in rigid body"); 
    if (nrigid[ibody] <= 0) error->all(FLERR,"Zero atoms in rigid body");

  // build list of joint connections and check for cycles and trees

  jointbuild();
  
  // delete temporary atom map
//  fprintf(screen, "delete temporary atom map..\n");
  if (mapflag) 
  {
//    atom->map_delete();
//    atom->map_style = 0;
  }

  // create POEMS instance 
  poems = new Workspace;
  
  // print statistics
  int nsum = 0;
  for (ibody = 0; ibody < nbody; ibody++) nsum += nrigid[ibody];
  
  if (me == 0) {
    if (screen)
      fprintf(screen,"%d clusters, %d bodies, %d joints, %d atoms\n",
	      ncluster,nbody,njoint,nsum);
    if (logfile)
      fprintf(logfile,"%d clusters, %d bodies, %d joints, %d atoms\n",
	      ncluster,nbody,njoint,nsum);
  }
  if(mydebug) fprintf(screen, "POEMS initialized!\n");
}

/* ----------------------------------------------------------------------
   free all memory for rigid bodies, joints, and POEMS
------------------------------------------------------------------------- */

FixPOEMS::~FixPOEMS()
{

  // if atom class still exists:
  //   unregister this fix so atom class doesn't invoke it any more

  if (atom) atom->delete_callback(id,0);

  // delete locally stored arrays

  memory->destroy(natom2body);
  memory->destroy(atom2body);
  memory->destroy(displace);

  // delete nbody-length arrays

  delete [] nrigid;
  delete [] masstotal;
  memory->destroy(xcm);
  memory->destroy(vcm);
  memory->destroy(fcm);
  memory->destroy(inertia);
  memory->destroy(ex_space);
  memory->destroy(ey_space);
  memory->destroy(ez_space);
  memory->destroy(angmom);
  memory->destroy(omega);
  memory->destroy(torque);

  memory->destroy(sum);
  memory->destroy(all);

  // delete joint arrays

  memory->destroy(jointbody);
  memory->destroy(xjoint);
  delete [] freelist;

  // delete POEMS object

  delete poems;
}

/* ---------------------------------------------------------------------- */

void FixPOEMS::post_create()
{

  if(mydebug) fprintf(screen, "POEMS::post_create()!\n");
  // register fixes for quantities to be saved to disk
  // see fix_property_atom.cpp for meaning of fixargs 
  if(!fix_xcm)
  {
        const char* fixarg[11];
        fixarg[0]="xcm";
        fixarg[1]="all";
        fixarg[2]="property/atom";
        fixarg[3]="xcm";
        fixarg[4]="vector";
        fixarg[5]="no";
        fixarg[6]="yes";
        fixarg[7]="no";
        fixarg[8]="0.";
	    fixarg[9]="0.";
	    fixarg[10]="0.";
	    fix_xcm = modify->add_fix_property_atom(11,const_cast<char**>(fixarg),style);
  }
  if(!fix_orientationEx)
  {
        const char* fixarg[11];
        fixarg[0]="orientationEx";
        fixarg[1]="all";
        fixarg[2]="property/atom";
        fixarg[3]="orientationEx";
        fixarg[4]="vector";
        fixarg[5]="no";
        fixarg[6]="yes";
        fixarg[7]="no";
        fixarg[8]="0.";
	    fixarg[9]="0.";
	    fixarg[10]="0.";
	    fix_orientationEx = 
	            modify->add_fix_property_atom(11,const_cast<char**>(fixarg),style);
  }

}

/* ---------------------------------------------------------------------- */

void FixPOEMS::updatePtrs()
{
  //double **x = atom->x;
  int nlocal = atom->nlocal;
  int i, ibody;


  //Set FixPropertyAtom for each atom in body
  for (i = 0; i < nlocal; i++) 
  {
    if (natom2body[i]) 
    {
      ibody = atom2body[i][0];
      fix_xcm->array_atom[i][0] = xcm[ibody][0];
      fix_xcm->array_atom[i][1] = xcm[ibody][1];
      fix_xcm->array_atom[i][2] = xcm[ibody][2];

     //Search joint that belongs to this body
     //Need to refresh first
     if(njoint)
     {
           fix_orientationEx->array_atom[i][0] = ex_space[ibody][0] ;
           fix_orientationEx->array_atom[i][1] = ex_space[ibody][1] ;
           fix_orientationEx->array_atom[i][2] = ex_space[ibody][2] ;
 

//                fprintf(screen, "i: %d orientationEx: %g %g %g \n", 
//                        i,
//                        fix_orientationEx->array_atom[i][0],
//                        fix_orientationEx->array_atom[i][1],
//                        fix_orientationEx->array_atom[i][2]); 
     }
    }
  }
}
/* ---------------------------------------------------------------------- */

int FixPOEMS::setmask()
{
  int mask = 0;
  mask |= INITIAL_INTEGRATE;
  mask |= FINAL_INTEGRATE;
  mask |= PRE_NEIGHBOR;
  mask |= POST_FORCE;
  mask |= INITIAL_INTEGRATE_RESPA;
  mask |= FINAL_INTEGRATE_RESPA;
  mask |= POST_FORCE_RESPA;  
  return mask;
}

/* ---------------------------------------------------------------------- */

void FixPOEMS::init()
{

  int i,ibody;

  // warn if more than one POEMS fix

  int count = 0;
  for (int i = 0; i < modify->nfix; i++)
    if (strcmp(modify->fix[i]->style,"poems") == 0) count++;
  if (count > 1 && comm->me == 0) error->warning(FLERR,"More than one fix poems");

  // error if npt,nph fix comes before rigid fix

  for (i = 0; i < modify->nfix; i++) {
    if (strcmp(modify->fix[i]->style,"npt") == 0) break;
    if (strcmp(modify->fix[i]->style,"nph") == 0) break;
  }
  if (i < modify->nfix) {
    for (int j = i; j < modify->nfix; j++)
      if (strcmp(modify->fix[j]->style,"poems") == 0)
	error->all(FLERR,"POEMS fix must come before NPT/NPH fix");
  }

  // timestep info

  dtv = update->dt;  
  dtf = 0.5 * update->dt * force->ftm2v;  
  dthalf = 0.5 * update->dt;

  // rRESPA info

  if (strstr(update->integrate_style,"respa")) {
    step_respa = ((Respa *) update->integrate)->step;
    nlevels_respa = ((Respa *) update->integrate)->nlevels;
  }

  // compute masstotal & center-of-mass xcm of each rigid body
  // only count joint atoms in 1st body

  int *type = atom->type;
  int *image = atom->image;
  double *rmass = atom->rmass;
  double *mass = atom->mass;
  double **x = atom->x;
  double **v = atom->v;
  int nlocal = atom->nlocal;

  double xprd = domain->xprd;
  double yprd = domain->yprd;
  double zprd = domain->zprd;

  int xbox,ybox,zbox;
  double massone;

  for (ibody = 0; ibody < nbody; ibody++)
    for (i = 0; i < 6; i++) sum[ibody][i] = 0.0;

  for (i = 0; i < nlocal; i++) {
    if (natom2body[i]) {
      ibody = atom2body[i][0];
      xbox = (image[i] & 1023) - 512;
      ybox = (image[i] >> 10 & 1023) - 512;
      zbox = (image[i] >> 20) - 512;
      if (rmass) 
     {
        massone = rmass[i];
     }
      else
     {
        massone = mass[type[i]];		
      }
      sum[ibody][0] += (x[i][0] + xbox*xprd) * massone;
      sum[ibody][1] += (x[i][1] + ybox*yprd) * massone;
      sum[ibody][2] += (x[i][2] + zbox*zprd) * massone;
      sum[ibody][3] += massone;
      sum[ibody][4] += massone *
	(v[i][0]*v[i][0] + v[i][1]*v[i][1] + v[i][2]*v[i][2]);
    }
  }

  MPI_Allreduce(sum[0],all[0],6*nbody,MPI_DOUBLE,MPI_SUM,world);

  total_ke = 0.0;
  for (ibody = 0; ibody < nbody; ibody++) {
    masstotal[ibody] = all[ibody][3];
    xcm[ibody][0] = all[ibody][0]/masstotal[ibody];
    xcm[ibody][1] = all[ibody][1]/masstotal[ibody];
    xcm[ibody][2] = all[ibody][2]/masstotal[ibody];
    total_ke += 0.5 * all[ibody][4];
  }

  // compute 6 moments of inertia of each body
  // only count joint atoms in 1st body
  // dx,dy,dz = coords relative to center-of-mass

  double dx,dy,dz;

  for (ibody = 0; ibody < nbody; ibody++)
    for (i = 0; i < 6; i++) sum[ibody][i] = 0.0;

  for (i = 0; i < nlocal; i++) {
    if (natom2body[i]) {
      ibody = atom2body[i][0];

      xbox = (image[i] & 1023) - 512;
      ybox = (image[i] >> 10 & 1023) - 512;
      zbox = (image[i] >> 20) - 512;
      dx = x[i][0] + xbox*xprd - xcm[ibody][0];
      dy = x[i][1] + ybox*yprd - xcm[ibody][1];
      dz = x[i][2] + zbox*zprd - xcm[ibody][2];
      if (rmass) 
     {
        massone = rmass[i];
     }
      else
     {
        massone = mass[type[i]];		
      }
    //HARDCODE: CYLINDER-MOMENT OF INERTIA
	//Cylinder in x-direction, each body consists of cylinders

	double radius=0.05;
	double length=1.0;
      sum[ibody][0] += 1.0/1.0 * massone * radius * radius;
      sum[ibody][1] += 1.0/12.0*massone * (3.0 * radius * radius + length * length);
      sum[ibody][2] += 1.0/12.0*massone * (3.0 * radius * radius + length * length);
      sum[ibody][3] -= 0;
      sum[ibody][4] -= 0;
      sum[ibody][5] -= 0;

    }
  }

  MPI_Allreduce(sum[0],all[0],6*nbody,MPI_DOUBLE,MPI_SUM,world);

  // inertia = 3 eigenvalues = principal moments of inertia
  // ex_space,ey_space,ez_space = 3 eigenvectors = principal axes of rigid body

  double **tensor,**evectors;
  memory->create(tensor,3,3,"fix_rigid:tensor");
  memory->create(evectors,3,3,"fix_rigid:evectors");

  int ierror;
  double ez0,ez1,ez2;

  for (ibody = 0; ibody < nbody; ibody++) {
    tensor[0][0] = all[ibody][0];
    tensor[1][1] = all[ibody][1];
    tensor[2][2] = all[ibody][2];
    tensor[0][1] = tensor[1][0] = all[ibody][3];
    tensor[1][2] = tensor[2][1] = all[ibody][4];
    tensor[0][2] = tensor[2][0] = all[ibody][5];
  
    ierror = jacobi(tensor,inertia[ibody],evectors);
    if (ierror) error->all(FLERR,"Insufficient Jacobi rotations for POEMS body");

    ex_space[ibody][0] = evectors[0][0];
    ex_space[ibody][1] = evectors[1][0];
    ex_space[ibody][2] = evectors[2][0];
    
    ey_space[ibody][0] = evectors[0][1];
    ey_space[ibody][1] = evectors[1][1];
    ey_space[ibody][2] = evectors[2][1];
    
    ez_space[ibody][0] = evectors[0][2];
    ez_space[ibody][1] = evectors[1][2];
    ez_space[ibody][2] = evectors[2][2];
    
    // if any principal moment < scaled EPSILON, error
    // this is b/c POEMS cannot yet handle degenerate bodies
  
    double max;
    max = MAX(inertia[ibody][0],inertia[ibody][1]);
    max = MAX(max,inertia[ibody][2]);
    
//    fprintf(screen, "max: %.3g; ibody: %d; inertia[ibody][i] %g %g %g \n", 
//                max,  
//                ibody,
//                inertia[ibody][0], inertia[ibody][1], inertia[ibody][2]);
  
    if (inertia[ibody][0] < EPSILON*max ||
	inertia[ibody][1] < EPSILON*max ||
	inertia[ibody][2] < EPSILON*max)
      error->all(FLERR,"Rigid body has degenerate moment of inertia");

    // enforce 3 evectors as a right-handed coordinate system
    // flip 3rd evector if needed
  
    ez0 = ex_space[ibody][1]*ey_space[ibody][2] -
      ex_space[ibody][2]*ey_space[ibody][1];
    ez1 = ex_space[ibody][2]*ey_space[ibody][0] -
      ex_space[ibody][0]*ey_space[ibody][2];
    ez2 = ex_space[ibody][0]*ey_space[ibody][1] -
      ex_space[ibody][1]*ey_space[ibody][0];
  
    if (ez0*ez_space[ibody][0] + ez1*ez_space[ibody][1] + 
	ez2*ez_space[ibody][2] < 0.0) {
      ez_space[ibody][0] = -ez_space[ibody][0];
      ez_space[ibody][1] = -ez_space[ibody][1];
      ez_space[ibody][2] = -ez_space[ibody][2];
    }
  }

  // free temporary memory
  
  memory->destroy(tensor);
  memory->destroy(evectors);

  // displace = initial atom coords in basis of principal axes
  // only set joint atoms relative to 1st body
  // set displace = 0.0 for atoms not in any rigid body

  for (i = 0; i < nlocal; i++) {
    if (natom2body[i]) {
      ibody = atom2body[i][0];

      xbox = (image[i] & 1023) - 512;
      ybox = (image[i] >> 10 & 1023) - 512;
      zbox = (image[i] >> 20) - 512;
      dx = x[i][0] + xbox*xprd - xcm[ibody][0];
      dy = x[i][1] + ybox*yprd - xcm[ibody][1];
      dz = x[i][2] + zbox*zprd - xcm[ibody][2];
      
      displace[i][0] = dx*ex_space[ibody][0] + dy*ex_space[ibody][1] +
	dz*ex_space[ibody][2];
      displace[i][1] = dx*ey_space[ibody][0] + dy*ey_space[ibody][1] +
	dz*ey_space[ibody][2];
      displace[i][2] = dx*ez_space[ibody][0] + dy*ez_space[ibody][1] +
	dz*ez_space[ibody][2];
    } else displace[i][0] = displace[i][1] = displace[i][2] = 0.0;
  }  

  // test for valid principal moments & axes
  // recompute moments of inertia around new axes
  // only count joint atoms in 1st body
  // 3 diagonal moments should equal principal moments
  // 3 off-diagonal moments should be 0.0
  // (ddx,ddy,ddz) is projection of atom within rigid body onto principal axes
  // 6 moments use  (ddx,ddy,ddz) displacements from principal axes

  for (ibody = 0; ibody < nbody; ibody++)
    for (i = 0; i < 6; i++) sum[ibody][i] = 0.0;

//  double ddx,ddy,ddz;

  for (i = 0; i < nlocal; i++) {
    if (natom2body[i]) {
      ibody = atom2body[i][0];

      xbox = (image[i] & 1023) - 512;
      ybox = (image[i] >> 10 & 1023) - 512;
      zbox = (image[i] >> 20) - 512;
      dx = x[i][0] + xbox*xprd - xcm[ibody][0];
      dy = x[i][1] + ybox*yprd - xcm[ibody][1];
      dz = x[i][2] + zbox*zprd - xcm[ibody][2];
      if (rmass) 
     {
        massone = rmass[i];
     }
      else
     {
        massone = mass[type[i]];		
      }

//      ddx = dx*ex_space[ibody][0] + dy*ex_space[ibody][1] + 
//	dz*ex_space[ibody][2];
//      ddy = dx*ey_space[ibody][0] + dy*ey_space[ibody][1] +
//	dz*ey_space[ibody][2];
//      ddz = dx*ez_space[ibody][0] + dy*ez_space[ibody][1] +
//	dz*ez_space[ibody][2];

//HARDCODE
	float radius=0.05;
	float length=1.0;
      sum[ibody][0] += 1./2. * massone * radius * radius;
      sum[ibody][1] += 1./12.*massone * (3. * radius * radius + length * length);
      sum[ibody][2] += 1./12.*massone * (3. * radius * radius + length * length);
      sum[ibody][3] -= 0.;
      sum[ibody][4] -= 0.;
      sum[ibody][5] -= 0.;
/*
      sum[ibody][0] += massone * (ddy*ddy + ddz*ddz);
      sum[ibody][1] += massone * (ddx*ddx + ddz*ddz);
      sum[ibody][2] += massone * (ddx*ddx + ddy*ddy);
      sum[ibody][3] -= massone * ddx*ddy;
      sum[ibody][4] -= massone * ddy*ddz;
      sum[ibody][5] -= massone * ddx*ddz;
*/
    }
  }
  
  MPI_Allreduce(sum[0],all[0],6*nbody,MPI_DOUBLE,MPI_SUM,world);
  
  for (ibody = 0; ibody < nbody; ibody++) {
    if (fabs(all[ibody][0]-inertia[ibody][0]) > TOLERANCE || 
	fabs(all[ibody][1]-inertia[ibody][1]) > TOLERANCE ||
	fabs(all[ibody][2]-inertia[ibody][2]) > TOLERANCE)
      error->all(FLERR,"Bad principal moments - type 1");
    if (fabs(all[ibody][3]) > TOLERANCE || 
	fabs(all[ibody][4]) > TOLERANCE ||
	fabs(all[ibody][5]) > TOLERANCE)
      error->all(FLERR,"Bad principal moments - type 2");
  }

  // find fix and assign values
  fix_xcm = static_cast<FixPropertyAtom*>(modify->find_fix_property("xcm","property/atom","vector",0,0,style));
  fix_orientationEx= static_cast<FixPropertyAtom*>(modify->find_fix_property("orientationEx","property/atom","vector",0,0,style));

}

/* ----------------------------------------------------------------------
   compute initial rigid body info
   make setup call to POEMS
------------------------------------------------------------------------- */

void FixPOEMS::setup(int vflag)
{

  int i,n,ibody;

  // vcm = velocity of center-of-mass of each rigid body
  // angmom = angular momentum of each rigid body
  // only count joint atoms in 1st body

  int *type = atom->type;
  int *image = atom->image;
  double *rmass = atom->rmass; 
  double *mass = atom->mass;
  double **x = atom->x;
  double **v = atom->v;
  int nlocal = atom->nlocal;

  double xprd = domain->xprd;
  double yprd = domain->yprd;
  double zprd = domain->zprd;

  int xbox,ybox,zbox;
  double massone,dx,dy,dz;

  for (ibody = 0; ibody < nbody; ibody++)
    for (i = 0; i < 6; i++) sum[ibody][i] = 0.0;

  for (i = 0; i < nlocal; i++) {
    if (natom2body[i]) {
      ibody = atom2body[i][0];
      if (rmass) 
     {
        massone = rmass[i];
     }
      else
     {
        massone = mass[type[i]];		
      }

      xbox = (image[i] & 1023) - 512;
      ybox = (image[i] >> 10 & 1023) - 512;
      zbox = (image[i] >> 20) - 512;
      dx = x[i][0] + xbox*xprd - xcm[ibody][0];
      dy = x[i][1] + ybox*yprd - xcm[ibody][1];
      dz = x[i][2] + zbox*zprd - xcm[ibody][2];

      sum[ibody][0] += v[i][0] * massone;
      sum[ibody][1] += v[i][1] * massone;
      sum[ibody][2] += v[i][2] * massone;
      sum[ibody][3] += dy * massone*v[i][2] - dz * massone*v[i][1];
      sum[ibody][4] += dz * massone*v[i][0] - dx * massone*v[i][2];
      sum[ibody][5] += dx * massone*v[i][1] - dy * massone*v[i][0]; 
    }
  }

  MPI_Allreduce(sum[0],all[0],6*nbody,MPI_DOUBLE,MPI_SUM,world);

  for (ibody = 0; ibody < nbody; ibody++) {
    vcm[ibody][0] = all[ibody][0]/masstotal[ibody];
    vcm[ibody][1] = all[ibody][1]/masstotal[ibody];
    vcm[ibody][2] = all[ibody][2]/masstotal[ibody];
    angmom[ibody][0] = all[ibody][3];
    angmom[ibody][1] = all[ibody][4];
    angmom[ibody][2] = all[ibody][5];  
  }

  // virial setup before call to set_v

  if (vflag) v_setup(vflag);
  else evflag = 0;

  // set velocities from angmom & omega

  for (ibody = 0; ibody < nbody; ibody++)
    omega_from_mq(angmom[ibody],ex_space[ibody],ey_space[ibody],
		  ez_space[ibody],inertia[ibody],omega[ibody]);
  set_v();

  // guestimate virial as 2x the set_v contribution

  if (vflag_global)
    for (n = 0; n < 6; n++) virial[n] *= 2.0;
  if (vflag_atom) {
    for (i = 0; i < nlocal; i++)
      for (n = 0; n < 6; n++)
	vatom[i][n] *= 2.0;
  }

  // use post_force() to compute initial fcm & torque

  post_force(vflag);

  // setup for POEMS

  poems->MakeSystem(nbody,masstotal,inertia,xcm,vcm,omega,
		    ex_space,ey_space,ez_space,
		    njoint,jointbody,xjoint,nfree,freelist,
		    dthalf,dtv,force->ftm2v,total_ke);

  // int currAtom=1;
/*   fprintf(screen, "masstotal: %g, currAtom: %d xcm %g %g %g,vcm %g %g %g ,omega %g %g %g, xjoint  %g %g %g\n",
		     masstotal[currAtom], currAtom,
             xcm[currAtom][0], xcm[currAtom][1], xcm[currAtom][2],
             vcm[currAtom][0], vcm[currAtom][1], vcm[currAtom][2],
             omega[currAtom][0], omega[currAtom][1], omega[currAtom][2],
             xjoint[currAtom][0],xjoint[currAtom][1],xjoint[currAtom][2]);
   fprintf(screen, "masstotal: %g, currAtom: %d dthalf %g,dtv %g ,force->ftm2v %g , total_ke %g\n",
		     masstotal[currAtom], currAtom,
             dthalf,
             dtv,
             force->ftm2v,
             total_ke);
*/

  //update fixes to report fibre data
  updatePtrs();

}

/* ----------------------------------------------------------------------
   update vcm,omega by 1/2 step and xcm,orientation by full step
   set x,v of body atoms accordingly
   ---------------------------------------------------------------------- */

void FixPOEMS::initial_integrate(int vflag)
{

  if(mydebug) fprintf(screen, "POEMS::initial_integrate()!\n");

  // perform POEMS integration

   poems->LobattoOne(xcm,vcm,omega,torque,fcm,ex_space,ey_space,ez_space);
/*
   int currAtom=1;
   fprintf(screen, "currAtom: %d xcm %g %g %g,vcm %g %g %g ,omega %g %g %g, torque  %g %g %g, fcm  %g %g %g\n",
            currAtom,
             xcm[currAtom][0], xcm[currAtom][1], xcm[currAtom][2],
             vcm[currAtom][0], vcm[currAtom][1], vcm[currAtom][2],
             omega[currAtom][0], omega[currAtom][1], omega[currAtom][2],
             torque[currAtom][0],torque[currAtom][1],torque[currAtom][2],
             fcm[currAtom][0],fcm[currAtom][1],fcm[currAtom][2]);

   fprintf(screen, "currAtom: %d e_space_1 %g %g %g,e_space_2 %g %g %g ,e_space_3 %g %g %g \n",
            currAtom,
            ex_space[currAtom][0],ey_space[currAtom][0],ez_space[currAtom][0],
            ex_space[currAtom][1],ey_space[currAtom][1],ez_space[currAtom][1],
            ex_space[currAtom][2],ey_space[currAtom][2],ez_space[currAtom][2]);*/

  // virial setup before call to set_xv

  if (vflag) v_setup(vflag);
  else evflag = 0;

  // set coords and velocities of atoms in rigid bodies
  set_xv();
}

/* ----------------------------------------------------------------------
   compute fcm,torque on each rigid body
   only count joint atoms in 1st body
------------------------------------------------------------------------- */

void FixPOEMS::post_force(int vflag)
{
  int i,ibody;
  int xbox,ybox,zbox;
  double dx,dy,dz;

  int *image = atom->image;
  double **x = atom->x;
  double **f = atom->f;
  int nlocal = atom->nlocal;
  
  double xprd = domain->xprd;
  double yprd = domain->yprd;
  double zprd = domain->zprd;
  
  for (ibody = 0; ibody < nbody; ibody++)
    for (i = 0; i < 6; i++) sum[ibody][i] = 0.0;
  
  for (i = 0; i < nlocal; i++) 
  {
      ibody = i;    //exploit fact that each atom is a body

      sum[ibody][0] += f[i][0];
      sum[ibody][1] += f[i][1];
      sum[ibody][2] += f[i][2];
      
      xbox = (image[i] & 1023) - 512;
      ybox = (image[i] >> 10 & 1023) - 512;
      zbox = (image[i] >> 20) - 512;
      dx = x[i][0] + xbox*xprd - xcm[ibody][0];
      dy = x[i][1] + ybox*yprd - xcm[ibody][1];
      dz = x[i][2] + zbox*zprd - xcm[ibody][2];
    
      sum[ibody][3] += dy*f[i][2] - dz*f[i][1];
      sum[ibody][4] += dz*f[i][0] - dx*f[i][2];
      sum[ibody][5] += dx*f[i][1] - dy*f[i][0];

//      fprintf(screen, "sum[%d]: %g %g %g %g %g %g \n", 
//                 ibody,
//                 sum[ibody][0],sum[ibody][1],sum[ibody][2],sum[ibody][3],sum[ibody][4],sum[ibody][5]);
  }
  
  MPI_Allreduce(sum[0],all[0],6*nbody,MPI_DOUBLE,MPI_SUM,world);

  for (ibody = 0; ibody < nbody; ibody++) {
    fcm[ibody][0] = all[ibody][0];
    fcm[ibody][1] = all[ibody][1];
    fcm[ibody][2] = all[ibody][2];
    torque[ibody][0] = all[ibody][3];
    torque[ibody][1] = all[ibody][4];
    torque[ibody][2] = all[ibody][5];
  }
}

/* ----------------------------------------------------------------------
   update vcm,omega by last 1/2 step
   set v of body atoms accordingly
------------------------------------------------------------------------- */

void FixPOEMS::final_integrate()
{

  // perform POEMS integration
  poems->LobattoTwo(vcm,omega,torque,fcm);

  // set velocities of atoms in rigid bodies
  // virial is already setup from initial_integrate

  set_v();
  
  //update fixes to report fibre data
  updatePtrs();
}

/* ---------------------------------------------------------------------- */

void FixPOEMS::initial_integrate_respa(int vflag, int ilevel, int iloop)
{
  dtv = step_respa[ilevel];
  dtf = 0.5 * step_respa[ilevel] * force->ftm2v;
  dthalf = 0.5 * step_respa[ilevel];

  if (ilevel == 0) initial_integrate(vflag);
  else final_integrate();
}

/* ---------------------------------------------------------------------- */

void FixPOEMS::post_force_respa(int vflag, int ilevel, int iloop)
{
  if (ilevel == nlevels_respa-1) post_force(vflag);
}

/* ---------------------------------------------------------------------- */

void FixPOEMS::final_integrate_respa(int ilevel, int iloop)
{
  dtf = 0.5 * step_respa[ilevel] * force->ftm2v;
  final_integrate();
}

/* ----------------------------------------------------------------------
   remap xcm of each rigid body back into periodic simulation box
   done during pre_neighbor so will be after call to pbc()
     and after fix_deform::pre_exchange() may have flipped box
   if don't do this, then atoms of a body which drifts far away
     from a triclinic box will be remapped back into box
     with huge displacements when the box tilt changes via set_x() 
   NOTE: cannot do this by changing xcm of each body in cluster
         or even 1st body in cluster
	 b/c POEMS library does not see xcm but only sets xcm
	 so remap needs to be coordinated with POEMS library
	 thus this routine does nothing for now
------------------------------------------------------------------------- */

void FixPOEMS::pre_neighbor() {}

/* ----------------------------------------------------------------------
   count # of degrees-of-freedom removed by fix_poems for atoms in igroup 
------------------------------------------------------------------------- */

int FixPOEMS::dof(int igroup)
{

  int groupbit = group->bitmask[igroup];

  // ncount = # of atoms in each rigid body that are also in group
  // only count joint atoms as part of first body

  int *mask = atom->mask;
  int nlocal = atom->nlocal;

  int *ncount = new int[nbody];
  for (int ibody = 0; ibody < nbody; ibody++) ncount[ibody] = 0;

  for (int i = 0; i < nlocal; i++)
    if (mask[i] & groupbit)
      if (natom2body[i]) ncount[atom2body[i][0]]++;

  int *nall = new int[nbody];
  MPI_Allreduce(ncount,nall,nbody,MPI_INT,MPI_SUM,world);

  // remove 3N - 6 dof for each rigid body if at least 2 atoms are in igroup

  int n = 0;
  for (int ibody = 0; ibody < nbody; ibody++)
    if (nall[ibody] > 2) n += 3*nall[ibody] - 6;

  // subtract 3 additional dof for each joint if atom is also in igroup

  int m = 0;
  for (int i = 0; i < nlocal; i++)
    if (natom2body[i] > 1 && (mask[i] & groupbit)) m += 3*(natom2body[i]-1);
  int mall;
  MPI_Allreduce(&m,&mall,1,MPI_INT,MPI_SUM,world);
  n += mall;

  // delete local memory

  delete [] ncount;
  delete [] nall;

  return n;
}

/* ----------------------------------------------------------------------
   adjust xcm of each cluster due to box deformation
   called by various fixes that change box size/shape
   flag = 0/1 means map from box to lamda coords or vice versa
   NOTE: cannot do this by changing xcm of each body in cluster
         or even 1st body in cluster
	 b/c POEMS library does not see xcm but only sets xcm
	 so deform needs to be coordinated with POEMS library
	 thus this routine does nothing for now
------------------------------------------------------------------------- */

void FixPOEMS::deform(int flag) {}

/* ---------------------------------------------------------------------- */

void FixPOEMS::readfile(char *file)
{


  FILE *fp;

  if (me == 0) {
    fp = fopen(file,"r");
    if (fp == NULL) {
      char str[128];
      sprintf(str,"Cannot open fix poems file %s",file);
      error->one(FLERR,str);
    }
  }

  nbody = 0;
  char *line = NULL;
  int maxline = 0;
  char *ptr;
  int nlocal = atom->nlocal;
  int i,id,nlen;

  while (1) {
    if (me == 0) nlen = readline(fp,&line,&maxline);
    MPI_Bcast(&nlen,1,MPI_INT,0,world);
    if (nlen == 0) break;
    MPI_Bcast(line,nlen,MPI_CHAR,0,world);

    ptr = strtok(line," ,\t\n\0");
    if (ptr == NULL || ptr[0] == '#') continue;
    ptr = strtok(NULL," ,\t\n\0");

    while (ptr) 
    { //FIXME?
      id = atoi(ptr);
      i = atom->map(id);
      if (i < 0 || i >= nlocal) continue;
      if (natom2body[i] < MAXBODY) atom2body[i][natom2body[i]] = nbody;
      natom2body[i]++;
      ptr = strtok(NULL," ,\t\n\0");
    }
    nbody++;
  }

  memory->destroy(line);
  fclose(fp);
}

/* ---------------------------------------------------------------------- */

int FixPOEMS::readline(FILE *fp, char **pline, int *pmaxline)
{


  int n = 0;
  char *line = *pline;
  int maxline = *pmaxline;

  while (1) {
    if (n+1 >= maxline) {
      maxline += DELTA;
      memory->grow(line,maxline,"fix_poems:line");
    }
    if (fgets(&line[n],maxline-n,fp) == NULL) {
      n = 0;
      break;
    }
    n = strlen(line);
    if (n < maxline-1 || line[n-1] == '\n') break;
  }

  *pmaxline = maxline;
  *pline = line;
  return n;
}

/* ----------------------------------------------------------------------
   build list of joints and error check for cycles and trees
------------------------------------------------------------------------- */

void FixPOEMS::jointbuild()
{

  int i; //,j;

  //WE DONT WANT JOINT ATOMS; RATHER JOINTS
  // convert atom2body into list of joint atoms on this proc
  // local_cpu_joint = # of joint atoms in this proc
  // an atom in N rigid bodies, infers N-1 joints between 1st body and others
  // mylist = [0],[1] = 2 body indices, [2] = global ID of joint atom
  double **x = atom->x;
//  int *tag = atom->tag;
  int nlocal = atom->nlocal;
 
  int local_cpu_joint = nlocal-1; //HARDCODE: number of joints on this CPU


  for (i = 0; i < nlocal; i++) {
    if (natom2body[i] <= 0) continue; //FIXME: changed from 1
    local_cpu_joint += natom2body[i]-1;
  }


  // jlist = mylist concatenated across all procs via MPI_Allgatherv
  MPI_Allreduce(&local_cpu_joint,&njoint,1,MPI_INT,MPI_SUM,world);
  int **jlist = NULL;
  if (njoint) memory->create(jlist,njoint,3,"poems:jlist");

  int nprocs;
  MPI_Comm_size(world,&nprocs);

  int *recvcounts = new int[nprocs];
  int tmp = 3*local_cpu_joint;
  MPI_Allgather(&tmp,1,MPI_INT,recvcounts,1,MPI_INT,world);

  int *displs = new int[nprocs];
  displs[0] = 0;
  for (i = 1; i < nprocs; i++) displs[i] = displs[i-1] + recvcounts[i-1];

  delete [] recvcounts;
  delete [] displs;

  // warning if no joints

  if (njoint == 0 && me == 0)
    error->warning(FLERR,"No joints between rigid bodies, use fix rigid instead");

  // sort joint list in ascending order by body indices
  // check for loops in joint connections between rigid bodies
  // check for trees = same body in more than 2 joints
  //sortlist(njoint,jlist);


  // allocate and setup joint arrays
  // jointbody stores body indices from 1 to Nbody to pass to POEMS
  // jointAtomID stores atom index of the joint atom
  // each proc sets myjoint if it owns joint atom
  // MPI_Allreduce gives all procs the xjoint coords

  jointbody = NULL;
  xjoint = NULL;
  double **myjoint = NULL;
  if (njoint) {
    memory->create(jointbody,njoint,2,"poems:jointbody");
    memory->create(xjoint,njoint,3,"poems:xjoint");
    memory->create(myjoint,njoint,3,"poems:myjoint");
  }


  for (i = 0; i < njoint; i++) 
  {
    
    //Create joints between atoms stored in nlocal (exactly in the middle)
    //Joint position
    myjoint[i][0]=(x[i][0]+x[i+1][0])/2;
    myjoint[i][1]=(x[i][1]+x[i+1][1])/2;
    myjoint[i][2]=(x[i][2]+x[i+1][2])/2;

    jointbody[i][0] = i+1;
    jointbody[i][1] = i+2;  

/*
    fprintf(screen, "me: %d,x[%d] %g %g %g,x[i+1] %g %g %g, myjoint[]: %g %g %g, jointbody[%d]: %d %d \n",
              me,
              i, 
              x[i][0],x[i][1],x[i][2],
              x[i+1][0],x[i+1][1],x[i+1][2],
              myjoint[i][0],myjoint[i][1],myjoint[i][2],
              i,
              jointbody[i][0],jointbody[i][1]);
*/
  }

  if (njoint)  
    MPI_Allreduce(myjoint[0],xjoint[0],3*njoint,MPI_DOUBLE,MPI_SUM,world);

  // compute freelist of nfree single unconnected bodies
  // POEMS could do this itself
  //HARDCODED
  nfree = 0;
  freelist = NULL;
  ncluster = 1;

  // free memory local to this routine
  memory->destroy(myjoint);

  if(mydebug)
  {
  for (i = 0; i < njoint; i++) 
  {
    fprintf(screen, "me: %d, xjoint[%d]: %g %g %g \n",
              me,
              i, 
              xjoint[i][0],xjoint[i][1],xjoint[i][2]);
  }
  }

}

/* ----------------------------------------------------------------------
  sort joint list (Numerical Recipes shell sort)
  sort criterion: sort on 1st body, if equal sort on 2nd body
------------------------------------------------------------------------- */

void FixPOEMS::sortlist(int n, int **list)
{

  int i,j,v0,v1,v2,flag;

  int inc = 1;
  while (inc <= n) inc = 3*inc + 1;

  do {
    inc /= 3;
    for (i = inc+1; i <= n; i++) {
      v0 = list[i-1][0];
      v1 = list[i-1][1];
      v2 = list[i-1][2];
      j = i;
      flag = 0;
      if (list[j-inc-1][0] > v0 || 
	  (list[j-inc-1][0] == v0 && list[j-inc-1][1] > v1)) flag = 1;
      while (flag) {
	list[j-1][0] = list[j-inc-1][0];
	list[j-1][1] = list[j-inc-1][1];
	list[j-1][2] = list[j-inc-1][2];
	j -= inc;
	if (j <= inc) break;
	flag = 0;
	if (list[j-inc-1][0] > v0 || 
	    (list[j-inc-1][0] == v0 && list[j-inc-1][1] > v1)) flag = 1;
      }
      list[j-1][0] = v0;
      list[j-1][1] = v1;
      list[j-1][2] = v2;
    }
  } while (inc > 1);
}

/* ----------------------------------------------------------------------
  check for cycles in list of joint connections between rigid bodies
  treat as graph: vertex = body, edge = joint between 2 bodies
------------------------------------------------------------------------- */

int FixPOEMS::loopcheck(int nvert, int nedge, int **elist)
{

  int i,j,k;

  // ecount[i] = # of vertices connected to vertex i via edge
  // elistfull[i][*] = list of vertices connected to vertex i

  int *ecount = new int[nvert];
  for (i = 0; i < nvert; i++) ecount[i] = 0;
  for (i = 0; i < nedge; i++) {
    ecount[elist[i][0]]++;
    ecount[elist[i][1]]++;
  }

  int emax = 0;
  for (i = 0; i < nvert; i++) emax = MAX(emax,ecount[i]);
  
  int **elistfull;
  memory->create(elistfull,nvert,emax,"poems:elistfull");
  for (i = 0; i < nvert; i++) ecount[i] = 0;
  for (i = 0; i < nedge; i++) {
    elistfull[elist[i][0]][ecount[elist[i][0]]++] = elist[i][1];
    elistfull[elist[i][1]][ecount[elist[i][1]]++] = elist[i][0];
  }

  // cycle detection algorithm
  // mark = 0/1 marking of each vertex, all initially unmarked
  // outer while loop:
  //   if all vertices are marked, no cycles, exit loop
  //   push an unmarked vertex on stack and mark it, parent is -1
  //   while stack is not empty:
  //     pop vertex I from stack
  //     loop over vertices J connected to I via edge
  //       if J is parent (vertex that pushed I on stack), skip it
  //       else if J is marked, a cycle is found, return 1
  //       else push J on stack and mark it, parent is I
  //   increment ncluster each time stack empties since that is new cluster

  int *parent = new int[nvert];
  int *mark = new int[nvert];
  for (i = 0; i < nvert; i++) mark[i] = 0;

  int nstack = 0;
  int *stack = new int[nvert];
  ncluster = 0;

  while (1) {
    for (i = 0; i < nvert; i++)
      if (mark[i] == 0) break;
    if (i == nvert) break;
    stack[nstack++] = i;
    mark[i] = 1;
    parent[i] = -1;

    while (nstack) {
      i = stack[--nstack];
      for (k = 0; k < ecount[i]; k++) {
	j = elistfull[i][k];
	if (j == parent[i]) continue;
	if (mark[j]) return 1;
	stack[nstack++] = j;
	mark[j] = 1;
	parent[j] = i;
      }
    }
    ncluster++;
  }

  // free memory local to this routine

  delete [] ecount;
  memory->destroy(elistfull);
  delete [] parent;
  delete [] mark;
  delete [] stack;

  return 0;
}

/* ----------------------------------------------------------------------
   compute evalues and evectors of 3x3 real symmetric matrix
   based on Jacobi rotations
   adapted from Numerical Recipes jacobi() function
------------------------------------------------------------------------- */

int FixPOEMS::jacobi(double **matrix, double *evalues, double **evectors)
{
  int i,j,k;
  double tresh,theta,tau,t,sm,s,h,g,c,b[3],z[3];
  
  for (i = 0; i < 3; i++) {
    for (j = 0; j < 3; j++) evectors[i][j] = 0.0;
    evectors[i][i] = 1.0;
  }
  for (i = 0; i < 3; i++) {
    b[i] = evalues[i] = matrix[i][i];
    z[i] = 0.0;
  }
  
  for (int iter = 1; iter <= MAXJACOBI; iter++) {
    sm = 0.0;
    for (i = 0; i < 2; i++)
      for (j = i+1; j < 3; j++)
	sm += fabs(matrix[i][j]);
    if (sm == 0.0) return 0;
    
    if (iter < 4) tresh = 0.2*sm/(3*3);
    else tresh = 0.0;
    
    for (i = 0; i < 2; i++) {
      for (j = i+1; j < 3; j++) {
	g = 100.0*fabs(matrix[i][j]);
	if (iter > 4 && fabs(evalues[i])+g == fabs(evalues[i])
	    && fabs(evalues[j])+g == fabs(evalues[j]))
	  matrix[i][j] = 0.0;
	else if (fabs(matrix[i][j]) > tresh) {
	  h = evalues[j]-evalues[i];
	  if (fabs(h)+g == fabs(h)) t = (matrix[i][j])/h;
	  else {
	    theta = 0.5*h/(matrix[i][j]);
	    t = 1.0/(fabs(theta)+sqrt(1.0+theta*theta));
	    if (theta < 0.0) t = -t;
	  }
	  c = 1.0/sqrt(1.0+t*t);
	  s = t*c;
	  tau = s/(1.0+c);
	  h = t*matrix[i][j];
	  z[i] -= h;
	  z[j] += h;
	  evalues[i] -= h;
	  evalues[j] += h;
	  matrix[i][j] = 0.0;
	  for (k = 0; k < i; k++) rotate(matrix,k,i,k,j,s,tau);
	  for (k = i+1; k < j; k++) rotate(matrix,i,k,k,j,s,tau);
	  for (k = j+1; k < 3; k++) rotate(matrix,i,k,j,k,s,tau);
	  for (k = 0; k < 3; k++) rotate(evectors,k,i,k,j,s,tau);
	}
      }
    }
    
    for (i = 0; i < 3; i++) {
      evalues[i] = b[i] += z[i];
      z[i] = 0.0;
    }
  }
  return 1;
}

/* ----------------------------------------------------------------------
   perform a single Jacobi rotation
------------------------------------------------------------------------- */

void FixPOEMS::rotate(double **matrix, int i, int j, int k, int l,
		      double s, double tau)
{
  double g = matrix[i][j];
  double h = matrix[k][l];
  matrix[i][j] = g-s*(h+g*tau);
  matrix[k][l] = h+s*(g-h*tau);
}

/* ----------------------------------------------------------------------
   compute omega from angular momentum
   w = omega = angular velocity in space frame
   wbody = angular velocity in body frame
   set wbody component to 0.0 if inertia component is 0.0
     otherwise body can spin easily around that axis
   project space-frame angular momentum onto body axes
     and divide by principal moments
------------------------------------------------------------------------- */

void FixPOEMS::omega_from_mq(double *m, double *ex, double *ey, double *ez,
			     double *inertia, double *w)
{
  double wbody[3];

  if (inertia[0] == 0.0) wbody[0] = 0.0;
  else wbody[0] = (m[0]*ex[0] + m[1]*ex[1] + m[2]*ex[2]) / inertia[0];
  if (inertia[1] == 0.0) wbody[1] = 0.0;
  else wbody[1] = (m[0]*ey[0] + m[1]*ey[1] + m[2]*ey[2]) / inertia[1];
  if (inertia[2] == 0.0) wbody[2] = 0.0;
  else wbody[2] = (m[0]*ez[0] + m[1]*ez[1] + m[2]*ez[2]) / inertia[2];

  w[0] = wbody[0]*ex[0] + wbody[1]*ey[0] + wbody[2]*ez[0];
  w[1] = wbody[0]*ex[1] + wbody[1]*ey[1] + wbody[2]*ez[1];
  w[2] = wbody[0]*ex[2] + wbody[1]*ey[2] + wbody[2]*ez[2];
}

/* ----------------------------------------------------------------------
   set space-frame coords and velocity of each atom in each rigid body
   x = Q displace + Xcm, mapped back to periodic box
   v = Vcm + (W cross (x - Xcm))
------------------------------------------------------------------------- */

void FixPOEMS::set_xv()
{
  int ibody;
  int xbox,ybox,zbox;
  double x0,x1,x2,v0,v1,v2,fc0,fc1,fc2,massone;
  double vr[6];

  int *image = atom->image;
  double **x = atom->x;
  double **v = atom->v;
  double **f = atom->f;
  double *rmass = atom->rmass; 
  double *mass = atom->mass; 
  int *type = atom->type;
  int nlocal = atom->nlocal;
  
  double xprd = domain->xprd;
  double yprd = domain->yprd;
  double zprd = domain->zprd;
  
  // set x and v of each atom
  // only set joint atoms for 1st rigid body they belong to

  for (int i = 0; i < nlocal; i++) {
    if (natom2body[i] == 0) continue;
    ibody = atom2body[i][0];

    xbox = (image[i] & 1023) - 512;
    ybox = (image[i] >> 10 & 1023) - 512;
    zbox = (image[i] >> 20) - 512;

    // save old positions and velocities for virial

    if (evflag) {
      x0 = x[i][0] + xbox*xprd;
      x1 = x[i][1] + ybox*yprd;
      x2 = x[i][2] + zbox*zprd;

      v0 = v[i][0];
      v1 = v[i][1];
      v2 = v[i][2];
    }

    // x = displacement from center-of-mass, based on body orientation
    // v = vcm + omega around center-of-mass

    x[i][0] = ex_space[ibody][0]*displace[i][0] +
      ey_space[ibody][0]*displace[i][1] + 
      ez_space[ibody][0]*displace[i][2];
    x[i][1] = ex_space[ibody][1]*displace[i][0] +
      ey_space[ibody][1]*displace[i][1] + 
      ez_space[ibody][1]*displace[i][2];
    x[i][2] = ex_space[ibody][2]*displace[i][0] +
      ey_space[ibody][2]*displace[i][1] + 
      ez_space[ibody][2]*displace[i][2];

    v[i][0] = omega[ibody][1]*x[i][2] - omega[ibody][2]*x[i][1] +
      vcm[ibody][0];
    v[i][1] = omega[ibody][2]*x[i][0] - omega[ibody][0]*x[i][2] +
      vcm[ibody][1];
    v[i][2] = omega[ibody][0]*x[i][1] - omega[ibody][1]*x[i][0] +
      vcm[ibody][2];
    
    // add center of mass to displacement
    // map back into periodic box via xbox,ybox,zbox

    x[i][0] += xcm[ibody][0] - xbox*xprd;
    x[i][1] += xcm[ibody][1] - ybox*yprd;
    x[i][2] += xcm[ibody][2] - zbox*zprd;

    // virial = unwrapped coords dotted into body constraint force
    // body constraint force = implied force due to v change minus f external
    // assume f does not include forces internal to body
    // 1/2 factor b/c final_integrate contributes other half
    // assume per-atom contribution is due to constraint force on that atom

    if (evflag) {
      if (rmass) 
     {
        massone = rmass[i];
     }
      else
     {
        massone = mass[type[i]];		
      }
      fc0 = massone*(v[i][0] - v0)/dtf - f[i][0];
      fc1 = massone*(v[i][1] - v1)/dtf - f[i][1];
      fc2 = massone*(v[i][2] - v2)/dtf - f[i][2]; 

      vr[0] = 0.5*fc0*x0;
      vr[1] = 0.5*fc1*x1;
      vr[2] = 0.5*fc2*x2;
      vr[3] = 0.5*fc1*x0;
      vr[4] = 0.5*fc2*x0;
      vr[5] = 0.5*fc2*x1;

      v_tally(1,&i,1.0,vr);
    }
  }
}

/* ----------------------------------------------------------------------
   set space-frame velocity of each atom in a rigid body
   v = Vcm + (W cross (x - Xcm))
------------------------------------------------------------------------- */

void FixPOEMS::set_v()
{
  int ibody;
  int xbox,ybox,zbox;
  double dx,dy,dz;
  double x0,x1,x2,v0,v1,v2,fc0,fc1,fc2,massone;
  double vr[6];

  double *rmass = atom->rmass; 
  double *mass = atom->mass; 
  double **f = atom->f;
  double **x = atom->x;
  double **v = atom->v;
  int *type = atom->type;
  int *image = atom->image;
  int nlocal = atom->nlocal;

  double xprd = domain->xprd;
  double yprd = domain->yprd;
  double zprd = domain->zprd;

  // set v of each atom
  // only set joint atoms for 1st rigid body they belong to

  for (int i = 0; i < nlocal; i++) {
    if (natom2body[i] == 0) continue;
    ibody = atom2body[i][0];

    dx = ex_space[ibody][0]*displace[i][0] +
      ey_space[ibody][0]*displace[i][1] + 
      ez_space[ibody][0]*displace[i][2];
    dy = ex_space[ibody][1]*displace[i][0] +
      ey_space[ibody][1]*displace[i][1] + 
      ez_space[ibody][1]*displace[i][2];
    dz = ex_space[ibody][2]*displace[i][0] +
      ey_space[ibody][2]*displace[i][1] + 
      ez_space[ibody][2]*displace[i][2];

    // save old velocities for virial

    if (evflag) {
      v0 = v[i][0];
      v1 = v[i][1];
      v2 = v[i][2];
    }

    v[i][0] = omega[ibody][1]*dz - omega[ibody][2]*dy + vcm[ibody][0];
    v[i][1] = omega[ibody][2]*dx - omega[ibody][0]*dz + vcm[ibody][1];
    v[i][2] = omega[ibody][0]*dy - omega[ibody][1]*dx + vcm[ibody][2];

    // virial = unwrapped coords dotted into body constraint force
    // body constraint force = implied force due to v change minus f external
    // assume f does not include forces internal to body
    // 1/2 factor b/c initial_integrate contributes other half
    // assume per-atom contribution is due to constraint force on that atom

    if (evflag) {
      if (rmass) 
     {
        massone = rmass[i];
     }
      else
     {
        massone = mass[type[i]];		
      }
      fc0 = massone*(v[i][0] - v0)/dtf - f[i][0];
      fc1 = massone*(v[i][1] - v1)/dtf - f[i][1];
      fc2 = massone*(v[i][2] - v2)/dtf - f[i][2]; 

      xbox = (image[i] & 1023) - 512;
      ybox = (image[i] >> 10 & 1023) - 512;
      zbox = (image[i] >> 20) - 512;

      x0 = x[i][0] + xbox*xprd;
      x1 = x[i][1] + ybox*yprd;
      x2 = x[i][2] + zbox*zprd;

      vr[0] = 0.5*fc0*x0;
      vr[1] = 0.5*fc1*x1;
      vr[2] = 0.5*fc2*x2;
      vr[3] = 0.5*fc1*x0;
      vr[4] = 0.5*fc2*x0;
      vr[5] = 0.5*fc2*x1;

      v_tally(1,&i,1.0,vr);
    }
  }
}

/* ----------------------------------------------------------------------
   allocate local atom-based arrays 
------------------------------------------------------------------------- */

void FixPOEMS::grow_arrays(int nmax)
{
  memory->grow(natom2body,nmax,"fix_poems:natom2body");
  memory->grow(atom2body,nmax,MAXBODY,"fix_poems:atom2body");
  memory->grow(displace,nmax,3,"fix_poems:displace");
}

/* ----------------------------------------------------------------------
   copy values within local atom-based arrays 
------------------------------------------------------------------------- */

void FixPOEMS::copy_arrays(int i, int j)
{
  natom2body[j] = natom2body[i];
  for (int k = 0; k < natom2body[j]; k++) atom2body[j][k] = atom2body[i][k];
  displace[j][0] = displace[i][0];
  displace[j][1] = displace[i][1];
  displace[j][2] = displace[i][2];
}

/* ----------------------------------------------------------------------
   memory usage of local atom-based arrays 
------------------------------------------------------------------------- */

double FixPOEMS::memory_usage()
{
  int nmax = atom->nmax;
  double bytes = nmax * sizeof(int);
  bytes += nmax*MAXBODY * sizeof(int);
  bytes += nmax*3 * sizeof(double);
  return bytes;
}

/* ----------------------------------------------------------------------
   pack values in local atom-based arrays for exchange with another proc 
------------------------------------------------------------------------- */

int FixPOEMS::pack_exchange(int i, double *buf)
{
  int m = 0;
  buf[m++] = static_cast<double> (natom2body[i]);
  for (int j = 0; j < natom2body[i]; j++) 
    buf[m++] = static_cast<double> (atom2body[i][j]);
  buf[m++] = displace[i][0];
  buf[m++] = displace[i][1];
  buf[m++] = displace[i][2];
  return m;
}

/* ----------------------------------------------------------------------
   unpack values in local atom-based arrays from exchange with another proc 
------------------------------------------------------------------------- */

int FixPOEMS::unpack_exchange(int nlocal, double *buf)
{

  int m = 0;
  natom2body[nlocal] = static_cast<int> (buf[m++]);
  for (int i = 0; i < natom2body[nlocal]; i++)
    atom2body[nlocal][i] = static_cast<int> (buf[m++]);
  displace[nlocal][0] = buf[m++];
  displace[nlocal][1] = buf[m++];
  displace[nlocal][2] = buf[m++];
  return m;
}

/* ---------------------------------------------------------------------- */

void FixPOEMS::reset_dt()
{
  dtv = update->dt;  
  dtf = 0.5 * update->dt * force->ftm2v;  
  dthalf = 0.5 * update->dt;
}
