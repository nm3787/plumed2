/* +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
   Copyright (c) 2012-2017 The plumed team
   (see the PEOPLE file at the root of the distribution for a list of names)

   See http://www.plumed.org for more information.

   This file is part of plumed, version 2.

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
#include "core/ActionPilot.h"
#include "core/ActionWithValue.h"
#include "core/ActionWithArguments.h"
#include "core/PlumedMain.h"
#include "core/ActionSet.h"
#include "core/ActionRegister.h"

//+PLUMEDOC GRIDCALC AVERAGE
/*
Calculate the ensemble average of a collective variable

The ensemble average for a non-periodic, collective variable, \f$s\f$ is given by the following expression:

\f[
\langle s \rangle = \frac{ \sum_{t'=0}^t w(t') s(t') }{ \sum_{t'=0}^t w(t') }
\f]

Here the sum runs over a the trajectory and \f$s(t')\f$ is used to denote the value of the collective variable
at time \f$t'\f$.  The final quantity evalulated is a weighted
average as the weights, \f$w(t')\f$, allow us to negate the effect any bias might have on the region of phase space
sampled by the system.  This is discussed in the section of the manual on \ref Analysis.

When the variable is periodic (e.g. \ref TORSION) and has a value, \f$s\f$, in \f$a \le s \le b\f$ the ensemble average is evaluated using:

\f[
\langle s \rangle = a + \frac{b - a}{2\pi} \arctan \left[ \frac{ \sum_{t'=0}^t w(t') \sin\left( \frac{2\pi [s(t')-a]}{b - a} \right) }{ \sum_{t'=0}^t w(t') \cos\left( \frac{2\pi [s(t')-a]}{b - a} \right) } \right]
\f]

\par Examples

The following example calculates the ensemble average for the distance between atoms 1 and 2
and output this to a file called COLVAR.  In this example it is assumed that no bias is acting
on the system and that the weights, \f$w(t')\f$ in the formulae above can thus all be set equal
to one.

\plumedfile
d1: DISTANCE ATOMS=1,2
d1a: AVERAGE ARG=d1
PRINT ARG=d1a FILE=colvar STRIDE=100
\endplumedfile

The following example calculates the ensemble average for the torsional angle involving atoms 1, 2, 3 and 4.
At variance with the previous example this quantity is periodic so the second formula in the above introduction
is used to calculate the average.  Furthermore, by using the CLEAR keyword we have specified that block averages
are to be calculated.  Consequently, after 100 steps all the information aquired thus far in the simulation is
forgotten and the process of averaging is begun again.  The quantities output in the colvar file are thus the
block averages taken over the first 100 frames of the trajectory, the block average over the second 100 frames
of trajectory and so on.

\plumedfile
t1: TORSION ATOMS=1,2,3,4
t1a: AVERAGE ARG=t1 CLEAR=100
PRINT ARG=t1a FILE=colvar STRIDE=100
\endplumedfile

This third example incorporates a bias.  Notice that the effect the bias has on the ensemble average is removed by taking
advantage of the \ref REWEIGHT_BIAS method.  The final ensemble averages output to the file are thus block ensemble averages for the
unbiased canononical ensemble at a temperature of 300 K.

\plumedfile
t1: TORSION ATOMS=1,2,3,4
RESTRAINT ARG=t1 AT=pi KAPPA=100.
ww: REWEIGHT_BIAS TEMP=300
t1a: AVERAGE ARG=t1 LOGWEIGHTS=ww CLEAR=100
PRINT ARG=t1a FILE=colvar STRIDE=100
\endplumedfile

*/
//+ENDPLUMEDOC

namespace PLMD {
namespace analysis {

class Average : 
public ActionPilot,
public ActionWithValue,
public ActionWithArguments {
private:
  enum {t,f,ndata} normalization;
  bool clearnextstep;
  unsigned clearstride;
  double lbound, pfactor;
public:
  static void registerKeywords( Keywords& keys );
  explicit Average( const ActionOptions& );
  void clearDerivatives( const bool& force=false ){}
  unsigned getNumberOfDerivatives() const ;
  bool allowComponentsAndValue() const { return true; }
  void getInfoForGridHeader( std::vector<std::string>& argn, std::vector<std::string>& min,
                             std::vector<std::string>& max, std::vector<unsigned>& nbin, std::vector<bool>& pbc ) const ;
  void getGridPointIndicesAndCoordinates( const unsigned& ind, std::vector<unsigned>& indices, std::vector<double>& coords ) const ;
  void calculate() {}
  void apply() {}
  void update();
};

PLUMED_REGISTER_ACTION(Average,"AVERAGE")

void Average::registerKeywords( Keywords& keys ) {
  Action::registerKeywords( keys );
  ActionPilot::registerKeywords( keys );
  ActionWithValue::registerKeywords( keys );
  ActionWithArguments::registerKeywords( keys ); keys.remove("ARG"); keys.use("UPDATE_FROM"); keys.use("UPDATE_UNTIL");
  keys.add("compulsory","ARG","the quantity that we are calculating an ensemble average for");
  keys.add("compulsory","STRIDE","1","the frequency with which the data should be collected and added to the quantity being averaged");
  keys.add("compulsory","CLEAR","0","the frequency with which to clear all the accumulated data.  The default value "
                                    "of 0 implies that all the data will be used and that the grid will never be cleared");
  keys.add("optional","LOGWEIGHTS","list of actions that calculates log weights that should be used to weight configurations when calculating averages");
  keys.add("compulsory","NORMALIZATION","true","This controls how the data is normalized it can be set equal to true, false or ndata.  The differences between "
                                               "these options are explained in the manual page for \\ref HISTOGRAM");
  keys.addOutputComponent("sin","default","this value is only added when the input argument is periodic.  These tempory values are required as with periodic arguments we need to use Berry phase averages.");
  keys.addOutputComponent("cos","default","this value is only added when the input argument is periodic.  These tempory values are required as With periodic arguments we need to use Berry phase averages.");
}

Average::Average( const ActionOptions& ao):
Action(ao),
ActionPilot(ao),
ActionWithValue(ao),
ActionWithArguments(ao),
clearnextstep(false),
lbound(0.0),pfactor(0.0)
{
  if( getNumberOfArguments()!=1 ) error("number of arguments to average should equal one");

  std::vector<std::string> wwstr; parseVector("LOGWEIGHTS",wwstr);
  if( wwstr.size()>0 ) log.printf("  reweighting using weights from ");
  std::vector<Value*> arg( getArguments() );
  for(unsigned i=0; i<wwstr.size(); ++i) {
    ActionWithValue* val = plumed.getActionSet().selectWithLabel<ActionWithValue*>(wwstr[i]);
    if( !val ) error("could not find value named");
    arg.push_back( val->copyOutput(val->getLabel()) );
    log.printf("%s ",wwstr[i].c_str() );
  }
  if( wwstr.size()>0 ) log.printf("\n");
  else log.printf("  weights are all equal to one\n");
  requestArguments( arg, false );
 
  // Read in clear instructions
  parse("CLEAR",clearstride);
  if( clearstride>0 ) {
    if( clearstride%getStride()!=0 ) error("CLEAR parameter must be a multiple of STRIDE");
    log.printf("  clearing average every %u steps \n",clearstride);
  }
  
  // Now read in the instructions for the normalization
  std::string normstr; parse("NORMALIZATION",normstr);
  if( normstr=="true" ) normalization=t;
  else if( normstr=="false" ) normalization=f;
  else if( normstr=="ndata" ) normalization=ndata;
  else error("invalid instruction for NORMALIZATION flag should be true, false, or ndata");

  // Create a value
  if( getPntrToArgument(0)->hasDerivatives() ) addValueWithDerivatives( getPntrToArgument(0)->getShape() );
  else addValue( getPntrToArgument(0)->getShape() );

  if( getPntrToArgument(0)->isPeriodic() ) {
      std::string min, max;
      getPntrToArgument(0)->getDomain( min, max ); setPeriodic( min, max );
      Tools::convert( min, lbound ); double ubound; Tools::convert( max, ubound );
      pfactor = ( ubound - lbound ) / (2*pi); 
      addComponent( "sin", getPntrToArgument(0)->getShape() ); componentIsNotPeriodic( "sin" );
      addComponent( "cos", getPntrToArgument(0)->getShape() ); componentIsNotPeriodic( "cos" );
      if( normalization!=f ){ getPntrToOutput(1)->setNorm(0.0); getPntrToOutput(2)->setNorm(0.0); }
  } else {
      setNotPeriodic();
      if( normalization!=f ) getPntrToOutput(0)->setNorm(0.0);
  }
}

unsigned Average::getNumberOfDerivatives() const {
  return getPntrToArgument(0)->getNumberOfDerivatives();
}

void Average::getInfoForGridHeader( std::vector<std::string>& argn, std::vector<std::string>& min,
                                    std::vector<std::string>& max, std::vector<unsigned>& nbin, std::vector<bool>& pbc ) const {
  plumed_dbg_assert( getNumberOfComponents()==1 && getPntrToOutput(0)->getRank()>0 && getPntrToOutput(0)->hasDerivatives() );
  (getPntrToArgument(0)->getPntrToAction())->getInfoForGridHeader( argn, min, max, nbin, pbc );
}

void Average::getGridPointIndicesAndCoordinates( const unsigned& ind, std::vector<unsigned>& indices, std::vector<double>& coords ) const {
  plumed_dbg_assert( getNumberOfComponents()==1 && getPntrToOutput(0)->getRank()>0 && getPntrToOutput(0)->hasDerivatives() );
  (getPntrToArgument(0)->getPntrToAction())->getGridPointIndicesAndCoordinates( ind, indices, coords );
}

void Average::update() {
  if( (clearstride!=1 && getStep()==0) || !onStep() ) return;

  if( clearnextstep ) {
      getPntrToOutput(0)->clearDerivatives();
      if( normalization!=f ){
         if( getPntrToArgument(0)->isPeriodic() ) {
             getPntrToOutput(1)->setNorm(0.0);
             getPntrToOutput(2)->setNorm(0.0);
         } else {
             getPntrToOutput(0)->setNorm(0.0);
         }
      }
      clearnextstep=false;
  }

  // Get weight information
  double cweight=1.0;
  if ( getNumberOfArguments()>1 ) {
    double sum=0; for(unsigned i=1; i<getNumberOfArguments(); ++i) sum+=getPntrToArgument(i)->get();
    cweight = exp( sum );
  } 

  // Accumulate normalization
  Value* arg0=getPntrToArgument(0); Value* val=getPntrToOutput(0);

  if( arg0->isPeriodic() ) {
     Value* valsin=getPntrToOutput(1); Value* valcos=getPntrToOutput(2); 
     // Accumulate normalization
     if( normalization==t ){ valsin->setNorm( valsin->getNorm() + cweight ); valcos->setNorm( valcos->getNorm() + cweight ); }
     else if( normalization==ndata ){ valsin->setNorm( valsin->getNorm() + 1.0 ); valcos->setNorm( valcos->getNorm() + 1.0 ); }
     // Now calcualte average
     for(unsigned i=0;i<arg0->getNumberOfValues();++i) {
         double tval = ( arg0->get(i) - lbound ) / pfactor;
         valsin->add( i, cweight*sin(tval) ); valcos->add( i, cweight*cos(tval) );
         val->set( i, lbound + pfactor*atan2( valsin->get(i), valcos->get(i)) );
     }
  } else {
     // Accumulate normalization
     if( normalization==t ) val->setNorm( val->getNorm() + cweight );
     else if( normalization==ndata ) val->setNorm( val->getNorm() + 1.0 ); 
     // Now accumulate average 
     for(unsigned i=0;i<arg0->getNumberOfValues();++i) {
         if( arg0->getRank()==0 && arg0->hasDerivatives() ) {
             for(unsigned j=0;j<val->getNumberOfDerivatives();++j) val->addDerivative( j, cweight*arg0->getDerivative( j ) );
         } else if( arg0->hasDerivatives() ) {
             unsigned nder=val->getNumberOfDerivatives(); val->add( i*(1+nder), cweight*arg0->get(i) );
             for(unsigned j=0;j<nder;++j) val->add( i*(1+nder)+1+j, cweight*arg0->getGridDerivative( i, j ) );
         } else {
             val->add( i, cweight*arg0->get(i) );
         }
     }
  }

  // Clear if required
  if( (clearstride>0 && getStep()%clearstride==0) ) clearnextstep=true;
}

}
}
