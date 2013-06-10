/* +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
   Copyright (c) 2013 The plumed team
   (see the PEOPLE file at the root of the distribution for a list of names)

   See http://www.plumed-code.org for more information.

   This file is part of plumed, version 2.0.

   plumed is free software: you can redistribute it and/or modify
   it under the terms of the GNU Lesser General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   plumed is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public License
   along with plumed.  If not, see <http://www.gnu.org/licenses/>.
+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++ */
#include "Bias.h"
#include "ActionRegister.h"
#include "tools/Grid.h"
#include "core/PlumedMain.h"
#include "core/Atoms.h"
#include "tools/Exception.h"
#include "core/FlexibleBin.h"
#include "tools/Matrix.h"
#include "tools/Random.h"
#include <string>
#include <cstring>
#include "tools/File.h"
#include "time.h"
#include <iostream>

#define DP2CUTOFF 6.25

using namespace std;


namespace PLMD{
namespace bias{

//+PLUMEDOC BIAS METAD 
/*
Used to performed MetaDynamics on one or more collective variables.

In a metadynamics simulations a history dependent bias composed of 
intermittently added Gaussian functions is added to the potential \cite metad.

\f[
V(\vec{s},t) = \sum_{ k \tau < t} W(k \tau)
\exp\left(
-\sum_{i=1}^{d} \frac{(s_i-s_i^{(0)}(k \tau))^2}{2\sigma_i^2}
\right).
\f]

This potential forces the system away from the kinetic traps in the potential energy surface
and out into the unexplored parts of the energy landscape. Information on the Gaussian
functions from which this potential is composed is output to a file called HILLS, which 
is used both the restart the calculation and to reconstruct the free energy as a function of the CVs. 
The free energy can be reconstructed from a metadynamics calculation because the final bias is given
by: 

\f[
V(\vec{s}) = -F(\vec(s))
\f]

During post processing the free energy can be calculated in this way using the \ref sum_hills
utility.

In the simplest possible implementation of a metadynamics calculation the expense of a metadynamics 
calculation increases with the length of the simulation as one has to, at every step, evaluate 
the values of a larger and larger number of Gaussians. To avoid this issue you can in plumed 2.0 
store the bias on a grid.  This approach is similar to that proposed in \cite babi+08jcp but has the 
advantage that the grid spacing is independent on the Gaussian width.

Another option that is available in plumed 2.0 is well-tempered metadynamics \cite Barducci:2008. In this
varient of metadynamics the heights of the Gaussian hills are rescaled at each step so the bias is now
given by:

\f[
V({s},t)= \sum_{t'=0,\tau_G,2\tau_G,\dots}^{t'<t} W e^{-V({s}({q}(t'),t')/\Delta T} \exp\left(
-\sum_{i=1}^{d} \frac{(s_i({q})-s_i({q}(t'))^2}{2\sigma_i^2}
\right),
\f]

This method ensures that the bias converges more smoothly.

Note that you can use here also the flexible gaussian approach  \cite Branduardi:2012dl
in which you can adapt the gaussian to the extent of Cartesian space covered by a variable or
to the space in collective variable covered in a given time. In this case the width of the deposited
gaussian potential is denoted by one value only that is a Cartesian space (ADAPTIVE=GEOM) or a time
(ADAPTIVE=DIFF). Note that a specific integration technique for the deposited gaussians
should be used in this case. Check the documentation for utility sum_hills.

\par Examples
The following input is for a standard metadynamics calculation using as
collective variables the distance between atoms 3 and 5
and the distance between atoms 2 and 4. The value of the CVs and
the metadynamics bias potential are written to the COLVAR file every 100 steps.
\verbatim
DISTANCE ATOMS=3,5 LABEL=d1
DISTANCE ATOMS=2,4 LABEL=d2
METAD ARG=d1,d2 SIGMA=0.2,0.2 HEIGHT=0.3 PACE=500 LABEL=restraint
PRINT ARG=d1,d2,restraint.bias STRIDE=100  FILE=COLVAR
\endverbatim
(See also \ref DISTANCE \ref PRINT).

\par
If you use adaptive Gaussians, with diffusion scheme where you use
a Gaussian that should cover the space of 20 timesteps in collective variables
\verbatim
DISTANCE ATOMS=3,5 LABEL=d1
DISTANCE ATOMS=2,4 LABEL=d2
METAD ARG=d1,d2 SIGMA=20 HEIGHT=0.3 PACE=500 LABEL=restraint ADAPTIVE=DIFF
PRINT ARG=d1,d2,restraint.bias STRIDE=100  FILE=COLVAR
\endverbatim

\par
If you use adaptive Gaussians, with geometrical scheme where you use
a Gaussian that should cover the space of 0.05 nm in Cartesian space
\verbatim
DISTANCE ATOMS=3,5 LABEL=d1
DISTANCE ATOMS=2,4 LABEL=d2
METAD ARG=d1,d2 SIGMA=0.05 HEIGHT=0.3 PACE=500 LABEL=restraint ADAPTIVE=GEOM
PRINT ARG=d1,d2,restraint.bias STRIDE=100  FILE=COLVAR
\endverbatim




*/
//+ENDPLUMEDOC

class MetaD : public Bias{

private:
  struct Gaussian {
   vector<double> center;
   vector<double> sigma;
   double height;
   bool   multivariate; // this is required to discriminate the one dimensional case 
   vector<double> invsigma;
   Gaussian(const vector<double> & center,const vector<double> & sigma,double height, bool multivariate ):
     center(center),sigma(sigma),height(height),multivariate(multivariate),invsigma(sigma){
       for(unsigned i=0;i<invsigma.size();++i)abs(invsigma[i])>1.e-20?invsigma[i]=1.0/invsigma[i]:0.; // to avoid troubles from zero element in flexible hills
     }
  };
  vector<double> sigma0_;
  vector<Gaussian> hills_;
  OFile hillsOfile_;
  Grid* BiasGrid_;
  bool storeOldGrids_;
  std::string gridfilename_;
  int wgridstride_; 
  bool grid_;
  double height0_;
  double biasf_;
  double temp_;
  int stride_;
  bool welltemp_;
  double* dp_;
  int adaptive_;
  FlexibleBin *flexbin;
  int mw_n_;
  string mw_dir_;
  int mw_id_;
  int mw_rstride_;
  vector<IFile*> ifiles;
  vector<string> ifilesnames;
  double uppI_;
  double lowI_;
  bool doInt_;
  bool isFirstStep;
  
  void   readGaussians(IFile*);
  bool   readChunkOfGaussians(IFile *ifile, unsigned n);
  void   writeGaussian(const Gaussian&,OFile&);
  void   addGaussian(const Gaussian&);
  double getHeight(const vector<double>&);
  double getBiasAndDerivatives(const vector<double>&,double* der=NULL);
  double evaluateGaussian(const vector<double>&, const Gaussian&,double* der=NULL);
  void   finiteDifferenceGaussian(const vector<double>&, const Gaussian&);
  vector<unsigned> getGaussianSupport(const Gaussian&);
  bool   scanOneHill(IFile *ifile,  vector<Value> &v, vector<double> &center, vector<double>  &sigma, double &height, bool &multivariate  );
  std::string fmt;

public:
  MetaD(const ActionOptions&);
  ~MetaD();
  void calculate();
  void update();
  static void registerKeywords(Keywords& keys);
  bool checkNeedsGradients()const{if(adaptive_==FlexibleBin::geometry){return true;}else{return false;}}
};

PLUMED_REGISTER_ACTION(MetaD,"METAD")

void MetaD::registerKeywords(Keywords& keys){
  Bias::registerKeywords(keys);
  keys.use("ARG");
  keys.add("compulsory","SIGMA","the widths of the Gaussian hills");
  keys.add("compulsory","HEIGHT","the heights of the Gaussian hills");
  keys.add("compulsory","PACE","the frequency for hill addition");
  keys.add("compulsory","FILE","HILLS","a file in which the list of added hills is stored");
  keys.add("optional","FMT","specify format for HILLS files (useful for decrease the number of digits in regtests)");
  keys.add("optional","BIASFACTOR","use well tempered metadynamics and use this biasfactor.  Please note you must also specify temp");
  keys.add("optional","TEMP","the system temperature - this is only needed if you are doing well-tempered metadynamics");
  keys.add("optional","GRID_MIN","the lower bounds for the grid");
  keys.add("optional","GRID_MAX","the upper bounds for the grid");
  keys.add("optional","GRID_BIN","the number of bins for the grid");
  keys.addFlag("GRID_SPARSE",false,"use a sparse grid to store hills");
  keys.addFlag("GRID_NOSPLINE",false,"don't use spline interpolation with grids");
  keys.add("optional","GRID_WSTRIDE","write the grid to a file every N steps");
  keys.add("optional","GRID_WFILE","the file on which to write the grid");
  keys.addFlag("STORE_GRIDS",false,"store all the grid files the calculation generates. They will be deleted if this keyword is not present");
  keys.add("optional","ADAPTIVE","use a geometric (=GEOM) or diffusion (=DIFF) based hills width scheme. Sigma is one number that has distance units or timestep dimensions");
  keys.add("optional","WALKERS_ID", "walker id");
  keys.add("optional","WALKERS_N", "number of walkers");
  keys.add("optional","WALKERS_DIR", "shared directory with the hills files from all the walkers");
  keys.add("optional","WALKERS_RSTRIDE","stride for reading hills files");
  keys.add("optional","LOWER_INTERVAL","monodimensional lower limit, below the limit the system will not fell the bias (when used together with grid SPLINES are automatically deactivated)");
  keys.add("optional","UPPER_INTERVAL","monodimensional upper limit, above the limit the system will not fell the bias (when used together with grid SPLINES are automatically deactivated)");
}

MetaD::~MetaD(){
  if(flexbin) delete flexbin;
  if(BiasGrid_) delete BiasGrid_;
  hillsOfile_.close();
  delete [] dp_;
  // close files
  for(int i=0;i<mw_n_;++i){
   if(ifiles[i]->isOpen()) ifiles[i]->close();
   delete ifiles[i];
  }
}

MetaD::MetaD(const ActionOptions& ao):
PLUMED_BIAS_INIT(ao),
// Grid stuff initialization
BiasGrid_(NULL), wgridstride_(0), grid_(false),
// Metadynamics basic parameters
height0_(0.0), biasf_(1.0), temp_(0.0),
stride_(0), welltemp_(false),
// Other stuff
dp_(NULL), adaptive_(FlexibleBin::none),
flexbin(NULL),
// Multiple walkers initialization
mw_n_(1), mw_dir_("./"), mw_id_(0), mw_rstride_(1),
// Interval initialization
uppI_(-1), lowI_(-1), doInt_(false),
isFirstStep(true)
{
  // parse the flexible hills
  string adaptiveoption;
  adaptiveoption="NONE";
  parse("ADAPTIVE",adaptiveoption);
  if (adaptiveoption=="GEOM"){
		  log.printf("  Uses Geometry-based hills width: sigma must be in distance units and only one sigma is needed\n");
		  adaptive_=FlexibleBin::geometry;	
  }else if (adaptiveoption=="DIFF"){
		  log.printf("  Uses Diffusion-based hills width: sigma must be in timesteps and only one sigma is needed\n");
		  adaptive_=FlexibleBin::diffusion;	
  }else if (adaptiveoption=="NONE"){
		  adaptive_=FlexibleBin::none;	
  }else{
		  error("I do not know this type of adaptive scheme");	
  }
  // parse the sigma
  parseVector("SIGMA",sigma0_);

  parse("FMT",fmt);

  // if you use normal sigma you need one sigma per argument 
  if (adaptive_==FlexibleBin::none){
         if( sigma0_.size()!=getNumberOfArguments() ) error("number of arguments does not match number of SIGMA parameters");
  }else{
  // if you use flexible hills you need one sigma  
         if(sigma0_.size()!=1){
        	 error("If you choose ADAPTIVE you need only one sigma according to your choice of type (GEOM/DIFF)");
         } 
         flexbin=new FlexibleBin(adaptive_,this,sigma0_[0]);
  }
  parse("HEIGHT",height0_);
  if( height0_<=0.0 ) error("error cannot add zero height or negative height hills");
  parse("PACE",stride_);
  if(stride_<=0 ) error("frequency for hill addition is nonsensical");
  string hillsfname="HILLS";
  parse("FILE",hillsfname);
  parse("BIASFACTOR",biasf_);
  if( biasf_<1.0 ) error("well tempered bias factor is nonsensical");
  parse("TEMP",temp_);
  if(biasf_>1.0){
   if(temp_==0.0) error("if you are doing well tempered metadynamics you must specify the temperature using TEMP");
   welltemp_=true;
  }

  // Grid Stuff
  vector<std::string> gmin(getNumberOfArguments());
  parseVector("GRID_MIN",gmin);
  if(gmin.size()!=getNumberOfArguments() && gmin.size()!=0) error("not enough values for GRID_MIN");
  vector<std::string> gmax(getNumberOfArguments());
  parseVector("GRID_MAX",gmax);
  if(gmax.size()!=getNumberOfArguments() && gmax.size()!=0) error("not enough values for GRID_MAX");
  vector<unsigned> gbin(getNumberOfArguments());
  parseVector("GRID_BIN",gbin);
  if(gbin.size()!=getNumberOfArguments() && gbin.size()!=0) error("not enough values for GRID_BIN");
  if( gmin.size()!=gmax.size() || gmin.size()!=gbin.size() ) error("GRID MIN was specified without either GRID_MAX or GRID_BIN");
  bool sparsegrid=false;
  parseFlag("GRID_SPARSE",sparsegrid);
  bool nospline=false;
  parseFlag("GRID_NOSPLINE",nospline);
  bool spline=!nospline;
  if(gbin.size()>0){grid_=true;}
  parse("GRID_WSTRIDE",wgridstride_);
  parse("GRID_WFILE",gridfilename_); 
  parseFlag("STORE_GRIDS",storeOldGrids_);
  if(grid_ && gridfilename_.length()>0){
    if(wgridstride_==0 ) error("frequency with which to output grid not specified use GRID_WSTRIDE");
  }
  if(grid_ && wgridstride_>0){
    if(gridfilename_.length()==0) error("grid filename not specified use GRID_WFILE"); 
  }

  // Multiple walkers
  parse("WALKERS_N",mw_n_);
  parse("WALKERS_ID",mw_id_);
  if(mw_n_<=mw_id_) error("walker ID should be a numerical value less than the total number of walkers");
  parse("WALKERS_DIR",mw_dir_);
  parse("WALKERS_RSTRIDE",mw_rstride_);

  // Inteval keyword
  parse("UPPER_INTERVAL",uppI_);
  parse("LOWER_INTERVAL",lowI_);

  checkRead();

  log.printf("  Gaussian width ");
  if (adaptive_==FlexibleBin::diffusion)log.printf(" (Note: The units of sigma are in timesteps) ");
  if (adaptive_==FlexibleBin::geometry)log.printf(" (Note: The units of sigma are in dist units) ");
  for(unsigned i=0;i<sigma0_.size();++i) log.printf(" %f",sigma0_[i]);
  log.printf("  Gaussian height %f\n",height0_);
  log.printf("  Gaussian deposition pace %d\n",stride_); 
  log.printf("  Gaussian file %s\n",hillsfname.c_str());
  if(welltemp_){log.printf("  Well-Tempered Bias Factor %f\n",biasf_);}
  if(uppI_!=lowI_) {
    log.printf("  Upper and Lower limits correction for the bias activated\n");
    if(sigma0_.size()!=1) error("Bias limits correction works only for monodimensional metadynamics!");
    if(uppI_<lowI_) error("The Upper limit must be greater than the Lower limit!");
    doInt_=true;
    spline=false;
  }
  if(grid_){
   log.printf("  Grid min");
   for(unsigned i=0;i<gmin.size();++i) log.printf(" %s",gmin[i].c_str() );
   log.printf("\n");
   log.printf("  Grid max");
   for(unsigned i=0;i<gmax.size();++i) log.printf(" %s",gmax[i].c_str() );
   log.printf("\n");
   log.printf("  Grid bin");
   for(unsigned i=0;i<gbin.size();++i) log.printf(" %d",gbin[i]);
   log.printf("\n");
   if(spline){log.printf("  Grid uses spline interpolation\n");}
   if(sparsegrid){log.printf("  Grid uses sparse grid\n");}
   if(wgridstride_>0){log.printf("  Grid is written on file %s with stride %d\n",gridfilename_.c_str(),wgridstride_);} 
  }
  if(mw_n_>1){
   log.printf("  %d multiple walkers active\n",mw_n_);
   log.printf("  walker id %d\n",mw_id_);
   log.printf("  reading stride %d\n",mw_rstride_);
   log.printf("  directory with hills files %s\n",mw_dir_.c_str());
  }

  addComponent("bias"); componentIsNotPeriodic("bias");

// for performance
  dp_ = new double[getNumberOfArguments()];

// initializing grid
  if(grid_){
   std::string funcl=getLabel() + ".bias";
   if(!sparsegrid){BiasGrid_=new Grid(funcl,getArguments(),gmin,gmax,gbin,spline,true);}
   else{BiasGrid_=new SparseGrid(funcl,getArguments(),gmin,gmax,gbin,spline,true);}
  }

// creating vector of ifile* for hills reading 
// open all files at the beginning and read Gaussians if restarting
  for(int i=0;i<mw_n_;++i){
   string fname;
   if(mw_n_>1) {
    stringstream out; out << i;
    fname = mw_dir_+"/"+hillsfname+"."+out.str();
   } else {
    fname = hillsfname;
   }
   IFile *ifile = new IFile();
   ifile->link(*this);
   ifiles.push_back(ifile);                                                             
   ifilesnames.push_back(fname);
   if(ifile->FileExist(fname)){
    ifile->open(fname);
    if(plumed.getRestart()){
     log.printf("  Restarting from %s:",ifilesnames[i].c_str());                  
     readGaussians(ifiles[i]);                                                    
    }
    ifiles[i]->reset(false);
    // close only the walker own hills file for later writing
    if(i==mw_id_) ifiles[i]->close();
   }
  }

// open hills file for writing
  hillsOfile_.link(*this);
  if(plumed.getRestart()) hillsOfile_.open(ifilesnames[mw_id_],"aw");
  else hillsOfile_.open(ifilesnames[mw_id_]);
  if(fmt.length()>0) hillsOfile_.fmtField(fmt);
  hillsOfile_.addConstantField("multivariate");
  hillsOfile_.setHeavyFlush();
// output periodicities of variables
  for(unsigned i=0;i<getNumberOfArguments();++i) hillsOfile_.setupPrintValue( getPntrToArgument(i) );

  log<<"  Bibliography "<<plumed.cite("Laio and Parrinello, PNAS 99, 12562 (2002)");
  if(welltemp_) log<<plumed.cite(
    "Barducci, Bussi, and Parrinello, Phys. Rev. Lett. 100, 020603 (2008)");
  log<<"\n";

}

void MetaD::readGaussians(IFile *ifile)
{
 unsigned ncv=getNumberOfArguments();
 vector<double> center(ncv);
 vector<double> sigma(ncv);
 double height;
 int nhills=0; 
 bool multivariate=false;

 std::vector<Value> tmpvalues;
 for(unsigned j=0;j<getNumberOfArguments();++j) tmpvalues.push_back( Value( this, getPntrToArgument(j)->getName(), false ) ); 

 while(scanOneHill(ifile,tmpvalues,center,sigma,height,multivariate)){;
  nhills++;
  if(welltemp_){height*=(biasf_-1.0)/biasf_;}
  addGaussian(Gaussian(center,sigma,height,multivariate));
 }     
 log.printf("      %d Gaussians read\n",nhills);
}

bool MetaD::readChunkOfGaussians(IFile *ifile, unsigned n)
{
 unsigned ncv=getNumberOfArguments();
 vector<double> center(ncv);
 vector<double> sigma(ncv);
 double height;
 unsigned nhills=0;
 bool multivariate=false;
 std::vector<Value> tmpvalues;
 for(unsigned j=0;j<getNumberOfArguments();++j) tmpvalues.push_back( Value( this, getPntrToArgument(j)->getName(), false ) ); 

 while(scanOneHill(ifile,tmpvalues,center,sigma,height,multivariate)){;
  if(welltemp_){height*=(biasf_-1.0)/biasf_;}
  addGaussian(Gaussian(center,sigma,height,multivariate));
  if(nhills==n){
      log.printf("      %d Gaussians read\n",nhills);
      return true;
  }
  nhills++;
 }     
 log.printf("      %d Gaussians read\n",nhills);
 return false;
}

void MetaD::writeGaussian(const Gaussian& hill, OFile&file){
  unsigned ncv=getNumberOfArguments();
  file.printField("time",getTimeStep()*getStep());
  for(unsigned i=0;i<ncv;++i){
    file.printField(getPntrToArgument(i),hill.center[i]);
  }
  if(hill.multivariate){
    hillsOfile_.printField("multivariate","true");
         Matrix<double> mymatrix(ncv,ncv);
         unsigned k=0;
         for(unsigned i=0;i<ncv;i++){
                for(unsigned j=i;j<ncv;j++){
                        mymatrix(i,j)=mymatrix(j,i)=hill.sigma[k]; // recompose the full inverse matrix
                        k++;
                }
         }
         // invert it 
         Matrix<double> invmatrix(ncv,ncv);
         Invert(mymatrix,invmatrix);
         // enforce symmetry
         for(unsigned i=0;i<ncv;i++){
                for(unsigned j=i;j<ncv;j++){
                        invmatrix(i,j)=invmatrix(j,i);
                }
         }

         // do cholesky so to have a "sigma like" number
         Matrix<double> lower(ncv,ncv);
         cholesky(invmatrix,lower); // now this , in band form , is similar to the sigmas
         // loop in band form 
         for (unsigned i=0;i<ncv;i++){
              for (unsigned j=0;j<ncv-i;j++){
                      file.printField("sigma_"+getPntrToArgument(j+i)->getName()+"_"+getPntrToArgument(j)->getName(),lower(j+i,j));
              }
         }
  } else {
    hillsOfile_.printField("multivariate","false");
    for(unsigned i=0;i<ncv;++i)
      file.printField("sigma_"+getPntrToArgument(i)->getName(),hill.sigma[i]);
  }
  double height=hill.height;
  if(welltemp_){height*=biasf_/(biasf_-1.0);}
  file.printField("height",height).printField("biasf",biasf_);
  if(mw_n_>1) file.printField("clock",int(time(0)));
  file.printField();
}

void MetaD::addGaussian(const Gaussian& hill)
{
 if(!grid_){hills_.push_back(hill);} 
 else{
  unsigned ncv=getNumberOfArguments();
  vector<unsigned> nneighb=getGaussianSupport(hill);
  vector<unsigned> neighbors=BiasGrid_->getNeighbors(hill.center,nneighb);
  vector<double> der(ncv);
  vector<double> xx(ncv);
  if(comm.Get_size()==1){
    for(unsigned i=0;i<neighbors.size();++i){
     unsigned ineigh=neighbors[i];
     for(unsigned j=0;j<ncv;++j){der[j]=0.0;}
     BiasGrid_->getPoint(ineigh,xx);
     double bias=evaluateGaussian(xx,hill,&der[0]);
     BiasGrid_->addValueAndDerivatives(ineigh,bias,der);
    } 
  } else {
    unsigned stride=comm.Get_size();
    unsigned rank=comm.Get_rank();
    vector<double> allder(ncv*neighbors.size(),0.0);
    vector<double> allbias(neighbors.size(),0.0);
    for(unsigned i=rank;i<neighbors.size();i+=stride){
     unsigned ineigh=neighbors[i];
     BiasGrid_->getPoint(ineigh,xx);
     allbias[i]=evaluateGaussian(xx,hill,&allder[ncv*i]);
    }
    comm.Sum(&allbias[0],allbias.size());
    comm.Sum(&allder[0],allder.size());
    for(unsigned i=0;i<neighbors.size();++i){
     unsigned ineigh=neighbors[i];
     for(unsigned j=0;j<ncv;++j){der[j]=allder[ncv*i+j];}
     BiasGrid_->addValueAndDerivatives(ineigh,allbias[i],der);
    }
  }
 }
}

vector<unsigned> MetaD::getGaussianSupport(const Gaussian& hill)
{
 vector<unsigned> nneigh;
 // traditional or flexible hill? 
 if(hill.multivariate){
	unsigned ncv=getNumberOfArguments();
	unsigned k=0;
	//log<<"------- GET GAUSSIAN SUPPORT --------\n"; 
	Matrix<double> mymatrix(ncv,ncv);
	for(unsigned i=0;i<ncv;i++){
		for(unsigned j=i;j<ncv;j++){
			mymatrix(i,j)=mymatrix(j,i)=hill.sigma[k]; // recompose the full inverse matrix
			k++;
		}
	}
        //
        // Reinvert so to have the ellipses 
        //
	Matrix<double> myinv(ncv,ncv);
	Invert(mymatrix,myinv);
	//log<<"INVERSE \n"; 
        //matrixOut(log,myinv);	
        // diagonalizes it
	Matrix<double> myautovec(ncv,ncv);
	vector<double> myautoval(ncv); //should I take this or their square root? 
	diagMat(myinv,myautoval,myautovec);
	double maxautoval;maxautoval=0.;
        unsigned ind_maxautoval;ind_maxautoval=ncv; 
	for (unsigned i=0;i<ncv;i++){
		if(myautoval[i]>maxautoval){maxautoval=myautoval[i];ind_maxautoval=i;}
        }  
	for (unsigned i=0;i<ncv;i++){
		double cutoff=sqrt(2.0*DP2CUTOFF)*abs(sqrt(maxautoval)*myautovec(i,ind_maxautoval));
		//log<<"AUTOVAL "<<myautoval[0]<<" COMP "<<abs(myautoval[0]*myautovec(i,0)) <<" CUTOFF "<<cutoff<<"\n";
	  	nneigh.push_back( static_cast<unsigned>(ceil(cutoff/BiasGrid_->getDx()[i])) );
        }
 }else{
	 for(unsigned i=0;i<getNumberOfArguments();++i){
	  double cutoff=sqrt(2.0*DP2CUTOFF)*hill.sigma[i];
	  nneigh.push_back( static_cast<unsigned>(ceil(cutoff/BiasGrid_->getDx()[i])) );
 	}
 }
	//log<<"------- END GET GAUSSIAN SUPPORT --------\n"; 
 return nneigh;
}

double MetaD::getBiasAndDerivatives(const vector<double>& cv, double* der)
{
 double bias=0.0;
 if(!grid_){
  unsigned stride=comm.Get_size();
  unsigned rank=comm.Get_rank();
  for(unsigned i=rank;i<hills_.size();i+=stride){
   bias+=evaluateGaussian(cv,hills_[i],der);
   //finite difference test 
   //finiteDifferenceGaussian(cv,hills_[i]);
  }
  comm.Sum(&bias,1);
  if(der) comm.Sum(&der[0],getNumberOfArguments());
 }else{
  if(der){
   vector<double> vder(getNumberOfArguments());
   bias=BiasGrid_->getValueAndDerivatives(cv,vder);

   if( ( doInt_ && cv[0] > lowI_ && cv[0] < uppI_) || (!doInt_) ) { // because interval can be used only with monodimensional metaD
     for(unsigned i=0;i<getNumberOfArguments();++i) {der[i]=vder[i];}
   }
  }else{
   bias=BiasGrid_->getValue(cv);
  }
 }
 return bias;
}

double MetaD::evaluateGaussian
 (const vector<double>& cv, const Gaussian& hill, double* der)
{
 double dp2=0.0;
 double bias=0.0;
 if(hill.multivariate){ 
    unsigned k=0;
    unsigned ncv=cv.size(); 
    // recompose the full sigma from the upper diag cholesky 
    Matrix<double> mymatrix(ncv,ncv);
    for(unsigned i=0;i<ncv;i++){
		for(unsigned j=i;j<ncv;j++){
			mymatrix(i,j)=mymatrix(j,i)=hill.sigma[k]; // recompose the full inverse matrix
			k++;
		}
    }

    for(unsigned i=0;i<cv.size();++i){
	double dp_i=difference(i,hill.center[i],cv[i]);
        dp_[i]=dp_i;
    	for(unsigned j=i;j<cv.size();++j){
                  if(i==j){ 
              	  	 dp2+=dp_i*dp_i*mymatrix(i,j)*0.5; 
                  }else{ 
   		 	 double dp_j=difference(j,hill.center[j],cv[j]);
              	  	 dp2+=dp_i*dp_j*mymatrix(i,j) ;
                  }
        }
    } 
    if(dp2<DP2CUTOFF){
     if( ( doInt_ && cv[0] > lowI_ && cv[0] < uppI_) || (!doInt_) ) {
       bias=hill.height*exp(-dp2);
       if(der){
        for(unsigned i=0;i<cv.size();++i){
                double tmp=0.0;
                k=i;
                for(unsigned j=0;j<cv.size();++j){
                                tmp+=   dp_[j]*mymatrix(i,j)*bias;
                        }
                        der[i]-=tmp;
                }   
       }
     }
    }
 }else{
    for(unsigned i=0;i<cv.size();++i){
     double dp=difference(i,hill.center[i],cv[i])*hill.invsigma[i];
     dp2+=dp*dp;
     dp_[i]=dp;
    }
    dp2*=0.5;
    if(dp2<DP2CUTOFF){
     if( ( doInt_ && cv[0] > lowI_ && cv[0] < uppI_) || (!doInt_) ) {
       bias=hill.height*exp(-dp2);
       if(der){
        for(unsigned i=0;i<cv.size();++i){der[i]+=-bias*dp_[i]*hill.invsigma[i];}
       }
     }
    }
 }
 return bias;
}

double MetaD::getHeight(const vector<double>& cv)
{
 double height=height0_;
 if(welltemp_){
    double vbias=getBiasAndDerivatives(cv);
    height=height0_*exp(-vbias/(plumed.getAtoms().getKBoltzmann()*temp_*(biasf_-1.0)));
 } 
 return height;
}

void MetaD::calculate()
{
  unsigned ncv=getNumberOfArguments();
  vector<double> cv(ncv);
  for(unsigned i=0;i<ncv;++i){cv[i]=getArgument(i);}

  double* der=new double[ncv];
  for(unsigned i=0;i<ncv;++i){der[i]=0.0;}
  double ene=getBiasAndDerivatives(cv,der);
  getPntrToComponent("bias")->set(ene);

// set Forces 
  for(unsigned i=0;i<ncv;++i){
   const double f=-der[i];
   setOutputForce(i,f);
  }

  delete [] der;
}

void MetaD::update(){
  vector<double> cv(getNumberOfArguments());
  vector<double> thissigma;
  bool multivariate;

  // adding hills criteria (could be more complex though)
  bool nowAddAHill; 
  if(getStep()%stride_==0 && !isFirstStep ){nowAddAHill=true;}else{nowAddAHill=false;isFirstStep=false;}

  for(unsigned i=0;i<cv.size();++i){cv[i]=getArgument(i);}

  // if you use adaptive, call the FlexibleBin 
  if (adaptive_!=FlexibleBin::none){
       flexbin->update(nowAddAHill);
       multivariate=true;
  }else{
       multivariate=false;
  };

  if(nowAddAHill){ // probably this can be substituted with a signal
   // add a Gaussian
   double height=getHeight(cv);
   // use normal sigma or matrix form? 
   if (adaptive_!=FlexibleBin::none){	
	thissigma=flexbin->getInverseMatrix(); // returns upper diagonal inverse 
	//cerr<<"ADDING HILLS "<<endl;	
   }else{
	thissigma=sigma0_;    // returns normal sigma
   }
   Gaussian newhill=Gaussian(cv,thissigma,height,multivariate);
   addGaussian(newhill);
// print on HILLS file
   writeGaussian(newhill,hillsOfile_);
  }
// dump grid on file
  if(wgridstride_>0&&getStep()%wgridstride_==0){
    OFile gridfile; gridfile.link(*this);
    if(!storeOldGrids_) remove( gridfilename_.c_str() );
    gridfile.open(gridfilename_);
    BiasGrid_->writeToFile(gridfile); 
    gridfile.close();
  }

// if multiple walkers and time to read Gaussians
 if(mw_n_>1 && getStep()%mw_rstride_==0){
   for(int i=0;i<mw_n_;++i){
    // don't read your own Gaussians
    if(i==mw_id_) continue;
    // if the file is not open yet 
    if(!(ifiles[i]->isOpen())){
     // check if it exists now and open it!
     if(ifiles[i]->FileExist(ifilesnames[i])) {
       ifiles[i]->open(ifilesnames[i]);
       ifiles[i]->reset(false);
     }
    // otherwise read the new Gaussians 
    } else {
     log.printf("  Reading hills from %s:",ifilesnames[i].c_str());
     readGaussians(ifiles[i]);
     ifiles[i]->reset(false);
    }
   }
 } 
}

void MetaD::finiteDifferenceGaussian
 (const vector<double>& cv, const Gaussian& hill)
{
 log<<"--------- finiteDifferenceGaussian: size "<<cv.size() <<"------------\n";
 // for each cv
 // first get the bias and the derivative
 vector<double> oldder(cv.size()); 
 vector<double> der(cv.size()); 
 vector<double> mycv(cv.size()); 
 mycv=cv; 
 double step=1.e-6;
 Random random; 
 // just displace a tiny bit
 for(unsigned i=0;i<cv.size();i++)log<<"CV "<<i<<" V "<<mycv[i]<<"\n";
 for(unsigned i=0;i<cv.size();i++)mycv[i]+=1.e-2*2*(random.RandU01()-0.5);
 for(unsigned i=0;i<cv.size();i++)log<<"NENEWWCV "<<i<<" V "<<mycv[i]<<"\n";
 double oldbias=evaluateGaussian(mycv,hill,&oldder[0]);
 for (unsigned i=0;i<mycv.size();i++){
               double delta=step*2*(random.RandU01()-0.5);
               mycv[i]+=delta;
               double newbias=evaluateGaussian(mycv,hill,&der[0]);		
               log<<"CV "<<i;
               log<<" ANAL "<<oldder[i]<<" NUM "<<(newbias-oldbias)/delta<<" DIFF "<<(oldder[i]-(newbias-oldbias)/delta)<<"\n";
               mycv[i]-=delta;
 }
 log<<"--------- END finiteDifferenceGaussian ------------\n";
}

/// takes a pointer to the file and a template string with values v and gives back the next center, sigma and height 
bool MetaD::scanOneHill(IFile *ifile,  vector<Value> &tmpvalues, vector<double> &center, vector<double>  &sigma, double &height , bool &multivariate  ){
  double dummy;
  multivariate=false;
  if(ifile->scanField("time",dummy)){
     unsigned ncv; ncv=tmpvalues.size();
     for(unsigned i=0;i<ncv;++i){
       ifile->scanField( &tmpvalues[i] );
       if( tmpvalues[i].isPeriodic() && ! getPntrToArgument(i)->isPeriodic() ){
          error("in hills file periodicity for variable " + tmpvalues[i].getName() + " does not match periodicity in input");
       } else if( tmpvalues[i].isPeriodic() ){
          std::string imin, imax; tmpvalues[i].getDomain( imin, imax );
          std::string rmin, rmax; getPntrToArgument(i)->getDomain( rmin, rmax );
          if( imin!=rmin || imax!=rmax ){
            error("in hills file periodicity for variable " + tmpvalues[i].getName() + " does not match periodicity in input");
          }
       }
       center[i]=tmpvalues[i].get();
     }
     // scan for multivariate label: record the actual file position so to eventually rewind 
     std::string sss;
     ifile->scanField("multivariate",sss);
     if(sss=="true") multivariate=true;
     else if(sss=="false") multivariate=false;
     else plumed_merror("cannot parse multivariate = "+ sss);
     if(multivariate){
        sigma.resize(ncv*(ncv+1)/2);
        Matrix<double> upper(ncv,ncv);
        Matrix<double> lower(ncv,ncv);
   	for (unsigned i=0;i<ncv;i++){
                 for (unsigned j=0;j<ncv-i;j++){
                         ifile->scanField("sigma_"+getPntrToArgument(j+i)->getName()+"_"+getPntrToArgument(j)->getName(),lower(j+i,j));
                         upper(j,j+i)=lower(j+i,j);
                 }
        }
        Matrix<double> mymult(ncv,ncv);       
        Matrix<double> invmatrix(ncv,ncv);       
	//log<<"Lower \n";
        //matrixOut(log,lower); 
	//log<<"Upper \n";
        //matrixOut(log,upper); 
        mult(lower,upper,mymult);          
	//log<<"Mult \n";
        //matrixOut(log,mymult); 
        // now invert and get the sigmas
        Invert(mymult,invmatrix);
	//log<<"Invert \n";
        //matrixOut(log,invmatrix); 
        // put the sigmas in the usual order: upper diagonal (this time in normal form and not in band form) 
        unsigned k=0;
   	for (unsigned i=0;i<ncv;i++){
   	       for (unsigned j=i;j<ncv;j++){
   	       	sigma[k]=invmatrix(i,j);
   	       	k++;
   	       }
   	}
     }else{
     	for(unsigned i=0;i<ncv;++i){
            ifile->scanField("sigma_"+getPntrToArgument(i)->getName(),sigma[i]);
        }
     }
     
     ifile->scanField("height",height);
     ifile->scanField("biasf",dummy);
     if(ifile->FieldExist("clock")) ifile->scanField("clock",dummy);
     ifile->scanField();
     return true;
  }else{ 
    return false; 
  }; 
}

}
}
