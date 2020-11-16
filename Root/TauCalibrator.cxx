/********************************************
 *
 * Interface to CP Tau calibration tool.
 *
 * F. Scutti (federico.scutti@cern.ch)
 ********************************************/


// c++ include(s):
#include <iostream>


// EDM include(s):
#include "xAODEventInfo/EventInfo.h"
#include "xAODTau/TauJetContainer.h"
#include "xAODTau/TauJet.h"
#include "xAODTau/TauxAODHelpers.h"
#include "xAODBase/IParticleHelpers.h"
#include "xAODBase/IParticleContainer.h"
#include "xAODBase/IParticle.h"
#include "AthContainers/ConstDataVector.h"
#include "AthContainers/DataVector.h"
#include "xAODCore/ShallowCopy.h"

// package include(s):
#include "xAODAnaHelpers/HelperFunctions.h"
#include "xAODAnaHelpers/HelperClasses.h"
#include "xAODAnaHelpers/TauCalibrator.h"
#include "PATInterfaces/CorrectionCode.h" // to check the return correction code status of tools


using HelperClasses::ToolName;

// this is needed to distribute the algorithm to the workers

TauCalibrator :: TauCalibrator (const std::string& name, ISvcLocator *pSvcLocator) :
    Algorithm(name, pSvcLocator, "TauCalibrator")
{
    declareProperty("inContainerName", m_inContainerName);
    declareProperty("outContainerName", m_outContainerName);
    declareProperty("RecommendationTag", m_RecommendationTag);
    declareProperty("applyMVATES", m_applyMVATES);
    declareProperty("applyCombinedTES", m_applyCombinedTES);
    declareProperty("setAFII", m_setAFII);
    declareProperty("sort", m_sort);
    declareProperty("inputAlgoSystNames", m_inputAlgoSystNames);
    declareProperty("outputAlgoSystNames", m_outputAlgoSystNames);
    declareProperty("writeSystToMetadata", m_writeSystToMetadata);
}


StatusCode TauCalibrator :: histInitialize ()
{
  // Here you do everything that needs to be done at the very
  // beginning on each worker node, e.g. create histograms and output
  // trees.  This method gets called before any input files are
  // connected.
  ANA_CHECK( xAH::Algorithm::algInitialize());
  return StatusCode::SUCCESS;
}


StatusCode TauCalibrator :: fileExecute ()
{
  // Here you do everything that needs to be done exactly once for every
  // single file, e.g. collect a list of all lumi-blocks processed
  return StatusCode::SUCCESS;
}


StatusCode TauCalibrator :: changeInput (bool /*firstFile*/)
{
  // Here you do everything you need to do when we change input files,
  // e.g. resetting branch addresses on trees.  If you are using
  // D3PDReader or a similar service this method is not needed.
  return StatusCode::SUCCESS;
}


StatusCode TauCalibrator :: initialize ()
{
  // Here you do everything that you need to do after the first input
  // file has been connected and before the first event is processed,
  // e.g. create additional histograms based on which variables are
  // available in the input files.  You can also create all of your
  // histograms and trees in here, but be aware that this method
  // doesn't get called if no events are processed.  So any objects
  // you create here won't be available in the output if you have no
  // input events.

  ANA_MSG_INFO( "Initializing TauCalibrator Interface... ");



  m_outAuxContainerName     = m_outContainerName + "Aux."; // the period is very important!
  // shallow copies are made with this output container name
  m_outSCContainerName      = m_outContainerName + "ShallowCopy";
  m_outSCAuxContainerName   = m_outSCContainerName + "Aux."; // the period is very important!

  if ( m_inContainerName.empty() ) {
    ANA_MSG_ERROR( "InputContainer is empty!");
    return StatusCode::FAILURE;
  }

  m_numEvent      = 0;
  m_numObject     = 0;

  // ************************************************
  //
  // initialize the TauAnalysisTools::TauSmearingTool
  //
  // ************************************************

  if (!m_RecommendationTag.empty()) ANA_CHECK(m_tauSmearingTool_handle.setProperty("RecommendationTag",m_RecommendationTag));
  ANA_CHECK(m_tauSmearingTool_handle.setProperty("ApplyMVATES",m_applyMVATES));
  ANA_CHECK(m_tauSmearingTool_handle.setProperty("ApplyCombinedTES",m_applyCombinedTES));

  ANA_CHECK(m_tauSmearingTool_handle.retrieve());
  ANA_MSG_DEBUG("Retrieved tool: " << m_tauSmearingTool_handle);

  // Get a list of recommended systematics for this tool
  //
  const CP::SystematicSet& recSyst = m_tauSmearingTool_handle->recommendedSystematics();

  ANA_MSG_INFO(" Initializing Tau Calibrator Systematics :");
  //
  // Make a list of systematics to be used, based on configuration input
  // Use HelperFunctions::getListofSystematics() for this!
  //
  m_systList = HelperFunctions::getListofSystematics( recSyst, m_systName, m_systVal, msg() );

  ANA_MSG_INFO("Will be using TauSmearingTool systematic:");
  auto SystTausNames = std::make_unique< std::vector< std::string > >();
  for ( const auto& syst_it : m_systList ) {
    if ( m_systName.empty() ) {
      ANA_MSG_INFO("\t Running w/ nominal configuration only!");
      break;
    }
    SystTausNames->push_back(syst_it.name());
    ANA_MSG_INFO("\t " << syst_it.name());
  }

  ANA_CHECK(evtStore()->record(std::move(SystTausNames), "taus_Syst"+m_name ));

  // Write output sys names
  if ( m_writeSystToMetadata ) {
    writeSystematicsListHist(m_systList, m_name);
  }

  ANA_MSG_INFO( "TauCalibrator Interface succesfully initialized!" );

  return StatusCode::SUCCESS;
}


StatusCode TauCalibrator :: execute ()
{
  // Here you do everything that needs to be done on every single
  // events, e.g. read input variables, apply cuts, and fill
  // histograms and trees.  This is where most of your actual analysis
  // code will go.

  ANA_MSG_DEBUG( "Applying Tau Calibration And Smearing ... ");

  m_numEvent++;

  const xAOD::EventInfo* eventInfo(nullptr);
  ANA_CHECK( evtStore()->retrieve(eventInfo, m_eventInfoContainerName) );


  // get the collections from TEvent or TStore
  //
  ANA_CHECK( evtStore()->retrieve(eventInfo, m_eventInfoContainerName) );
  const xAOD::TauJetContainer* inTaus(nullptr);
  ANA_CHECK( evtStore()->retrieve(inTaus, m_inContainerName) );

  // loop over available systematics - remember syst == EMPTY_STRING --> baseline
  // prepare a vector of the names of CDV containers
  // must be a pointer to be recorded in TStore
  //
  auto vecOutContainerNames = std::make_unique< std::vector< std::string > >();

  for ( const auto& syst_it : m_systList ) {

    std::string outSCContainerName(m_outSCContainerName);
    std::string outSCAuxContainerName(m_outSCAuxContainerName);
    std::string outContainerName(m_outContainerName);

    // always append the name of the variation, including nominal which is an empty string
    //
    outSCContainerName    += syst_it.name();
    outSCAuxContainerName += syst_it.name();
    outContainerName      += syst_it.name();
    vecOutContainerNames->push_back( syst_it.name() );

    // apply syst
    //
    if ( m_tauSmearingTool_handle->applySystematicVariation(syst_it) != CP::SystematicCode::Ok ) {
      ANA_MSG_ERROR( "Failed to configure TauSmearingTool for systematic " << syst_it.name());
      return StatusCode::FAILURE;
    }

    // create shallow copy for calibration - one per syst
    //
    std::pair< xAOD::TauJetContainer*, xAOD::ShallowAuxContainer* > calibTausSC = xAOD::shallowCopyContainer( *inTaus );
    // create ConstDataVector to be eventually stored in TStore
    //
    ConstDataVector<xAOD::TauJetContainer>* calibTausCDV = new ConstDataVector<xAOD::TauJetContainer>(SG::VIEW_ELEMENTS);
    calibTausCDV->reserve( calibTausSC.first->size() );

    // now calibrate!
    //
    unsigned int idx(0);
    if ( isMC() ) {

      for ( auto tauSC_itr : *(calibTausSC.first) ) {

	ANA_MSG_DEBUG( "  uncailbrated tau " << idx << ", pt = " << tauSC_itr->pt()*1e-3 << " GeV");
	if(xAOD::TauHelpers::getTruthParticle(tauSC_itr)){
	  if ( m_tauSmearingTool_handle->applyCorrection(*tauSC_itr) == CP::CorrectionCode::Error ) {  // Can have CorrectionCode values of Ok, OutOfValidityRange, or Error. Here only checking for Error.
	    ANA_MSG_WARNING( "TauSmearingTool returned Error CorrectionCode");		  // If OutOfValidityRange is returned no modification is made and the original tau values are taken.
	  }
	}

        ANA_MSG_DEBUG( "  corrected tau pt = " << tauSC_itr->pt()*1e-3 << " GeV");

	++idx;

      } // close calibration loop
    }

    ANA_MSG_DEBUG( "setOriginalObjectLink");
    if ( !xAOD::setOriginalObjectLink(*inTaus, *(calibTausSC.first)) ) {
      ANA_MSG_ERROR( "Failed to set original object links -- MET rebuilding cannot proceed.");
    }

    // save pointers in ConstDataVector with same order
    //
    ANA_MSG_DEBUG( "makeSubsetCont");
    ANA_CHECK( HelperFunctions::makeSubsetCont(calibTausSC.first, calibTausCDV, msg()));
    ANA_MSG_DEBUG( "done makeSubsetCont");

    // sort after coping to CDV
    if ( m_sort ) {
      ANA_MSG_DEBUG( "sorting");
      std::sort( calibTausCDV->begin(), calibTausCDV->end(), HelperFunctions::sort_pt );
    }

    // add SC container to TStore
    //
    ANA_MSG_DEBUG( "recording calibTausSC");
    ANA_CHECK( evtStore()->record( calibTausSC.first,  outSCContainerName  ));
    ANA_CHECK( evtStore()->record( calibTausSC.second, outSCAuxContainerName ));

    //
    // add ConstDataVector to TStore
    //
    ANA_MSG_DEBUG( "record calibTausCDV");
    ANA_CHECK( evtStore()->record( calibTausCDV, outContainerName));

  } // close loop on systematics

  // add vector<string container_names_syst> to TStore
  //
  ANA_MSG_DEBUG( "record m_outputAlgoSystNames");
  ANA_CHECK( evtStore()->record( std::move(vecOutContainerNames), m_outputAlgoSystNames));

  // look what we have in TStore
  //
  if(msgLvl(MSG::VERBOSE)) evtStore()->print();

  ANA_MSG_DEBUG( "Left ");
  return StatusCode::SUCCESS;

}


StatusCode TauCalibrator :: finalize ()
{
  // This method is the mirror image of initialize(), meaning it gets
  // called after the last event has been processed on the worker node
  // and allows you to finish up any objects you created in
  // initialize() before they are written to disk.  This is actually
  // fairly rare, since this happens separately for each worker node.
  // Most of the time you want to do your post-processing on the
  // submission node after all your histogram outputs have been
  // merged.  This is different from histFinalize() in that it only
  // gets called on worker nodes that processed input events.

  return StatusCode::SUCCESS;
}

StatusCode TauCalibrator :: histFinalize ()
{
  // This method is the mirror image of histInitialize(), meaning it
  // gets called after the last event has been processed on the worker
  // node and allows you to finish up any objects you created in
  // histInitialize() before they are written to disk.  This is
  // actually fairly rare, since this happens separately for each
  // worker node.  Most of the time you want to do your
  // post-processing on the submission node after all your histogram
  // outputs have been merged.  This is different from finalize() in
  // that it gets called on all worker nodes regardless of whether
  // they processed input events.

  ANA_MSG_INFO( "Calling histFinalize");
  ANA_CHECK( xAH::Algorithm::algFinalize());
  return StatusCode::SUCCESS;
}
