/* model.cc	-- Greg Franks Mon Feb  3 2003
 *
 * $Id: model.cc 14142 2020-11-26 16:40:03Z greg $
 *
 * Load, slice, and dice the lqn model.
 */

#include "lqn2ps.h"
#if HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <algorithm>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <cstring>
#include <errno.h>
#include <cmath>
#include <sstream>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#if HAVE_FLOAT_H
#include <float.h>
#endif
#if HAVE_VALUES_H
#include <values.h>
#endif
#if defined(X11_OUTPUT)
#include <sys/stat.h>
#endif
#include <lqio/dom_document.h>
#include <lqio/srvn_output.h>
#include <lqio/srvn_spex.h>
#include "model.h"
#include "entry.h"
#include "activity.h"
#include "actlist.h"
#include "task.h"
#include "call.h"
#include "share.h"
#include "open.h"
#include "group.h"
#include "processor.h"
#include "layer.h"
#include "key.h"
#include "errmsg.h"
#include "graphic.h"
#include "label.h"
#include "runlqx.h"

class CommentManip {
public:
    CommentManip( std::ostream& (*ff)(std::ostream&, const char *, const LQIO::DOM::ExternalVariable& ), const char * prefix, const LQIO::DOM::ExternalVariable& var )
	: f(ff), _prefix(prefix), _var(var) {}
private:
    std::ostream& (*f)( std::ostream&, const char *, const LQIO::DOM::ExternalVariable& );
    const char * _prefix;
    const LQIO::DOM::ExternalVariable& _var;

    friend std::ostream& operator<<(std::ostream & os, const CommentManip& m )
	{ return m.f(os,m._prefix,m._var); }
};


Model * Model::__model = 0;

unsigned Model::iteration_limit   = 50;
unsigned Model::print_interval    = 10;
double Model::convergence_value   = 0.00001;
double Model::underrelaxation     = 0.9;

unsigned Model::openArrivalCount  = 0;
unsigned Model::forwardingCount	  = 0;
unsigned Model::rendezvousCount[MAX_PHASES+1];
unsigned Model::sendNoReplyCount[MAX_PHASES+1];
unsigned Model::phaseCount[MAX_PHASES+1];
int Model::maxModelNumber = 1;

/* input */
bool Model::deterministicPhasesPresent	= false;
bool Model::maxServiceTimePresent	= false;
bool Model::nonExponentialPhasesPresent	= false;
bool Model::thinkTimePresent		= false;
/* output */
bool Model::boundsPresent		= false;
bool Model::serviceExceededPresent	= false;
bool Model::variancePresent		= false;
bool Model::histogramPresent		= false;

Model::Stats Model::stats[Model::N_STATS];

static CommentManip print_comment( const char * aPrefix, const LQIO::DOM::ExternalVariable& );
static DoubleManip to_inches( const double );

const char * Model::XMLSchema_instance = "http://www.w3.org/2001/XMLSchema-instance";
inline void create_pragma( const std::pair<std::string,std::string>&p ) { pragma( p.first, p.second ); }

/*
 * Compare two entities by their submodel.
 */

struct lt_submodel
{
    bool operator()(const Entity * e1, const Entity * e2) const
	{
	    return (e1->level() < e2->level())
		|| (e1->level() == e2->level() && (!dynamic_cast<const LQIO::DOM::Entity *>(e2->getDOM()) || (dynamic_cast<const LQIO::DOM::Entity *>(e1->getDOM()) && (dynamic_cast<const LQIO::DOM::Entity *>(e1->getDOM())->getId() < dynamic_cast<const LQIO::DOM::Entity *>(e2->getDOM())->getId()))));
	}
};

/* ------------------------ Constructors etc. ------------------------- */

Model::Model( LQIO::DOM::Document * document, const std::string& input_file_name, const std::string& output_file_name, unsigned int numberOfLayers )
    : _layers(numberOfLayers+1),
      _key(nullptr),
      _label(nullptr),
      _total(),
      _document(document),
      _inputFileName( input_file_name ),
      _outputFileName( output_file_name ),
      _login(),
      _modelNumber(1),
      _scaling(1.0)
{
    __model = this;
    /* Check for more than one instance */

    if ( graphical_output() && Flags::print[KEY].value.i != 0 ) {
	_key = new Key;
    }
    if ( graphical_output() && (Flags::print[MODEL_COMMENT].value.b || Flags::print[SOLVER_INFO].value.b) ) {
	_label = Label::newLabel();
    }

    stats[TOTAL_ENTRIES].name( "Entries" ).accumulate( &Model::nEntries );
    stats[TOTAL_LAYERS].name( "Layers" ).accumulate( &Model::nLayers );
    stats[TOTAL_PROCESSORS].name( "Processors" ).accumulate( &Model::nProcessors );
    stats[TOTAL_TASKS].name( "Tasks" ).accumulate( &Model::nTasks );
    stats[TOTAL_INFINITE_SERVERS].name( "Infinite Servers" ).accumulate( &Model::nInfiniteServers );
    stats[TOTAL_MULTISERVERS].name( "MultiServers" ).accumulate( &Model::nMultiServers );
    stats[ENTRIES_PER_TASK].name( "Entries per Task" );
    stats[OPEN_ARRIVALS_PER_ENTRY].name( "Open Arrivals per Entry" );
    stats[OPEN_ARRIVAL_RATE_PER_ENTRY].name( "Total Open Arrival Rate" );
    stats[PHASES_PER_ENTRY].name( "Number of Phases per Entry" );
    stats[RNVS_PER_ENTRY].name( "Rendezvous per Entry" );
    stats[RNV_RATE_PER_CALL].name( "RNV Rate per Call" );
    stats[SERVICE_TIME_PER_PHASE].name( "Service Time per Phase" );
    stats[SNRS_PER_ENTRY].name( "Send-no-replies per Entry" );
    stats[SNR_RATE_PER_CALL].name( "SNR Rate per Call" );
    stats[FORWARDING_PER_ENTRY].name( "Forwarding per Entry" );
    stats[FORWARDING_PROBABILITY_PER_CALL].name( "Forwarding Probabilty per Call" );
    stats[TASKS_PER_LAYER].name( "Tasks per Layer" );

    /* input */
    deterministicPhasesPresent	= false;
    maxServiceTimePresent	= false;
    nonExponentialPhasesPresent	= false;
    thinkTimePresent		= false;
    /* output */
    boundsPresent		= false;
    histogramPresent		= false;
    serviceExceededPresent	= false;
    variancePresent		= false;
}


/*
 * Delete the model including stuff allocated externally.
 */

Model::~Model()
{
    for ( std::set<Share *>::const_iterator s = Share::__share.begin(); s != Share::__share.end(); ++s ) {
	delete *s;
    }
    Share::__share.clear();

    for ( std::set<Task *>::const_iterator t = Task::__tasks.begin(); t != Task::__tasks.end(); ++t ) {
	delete *t;
    }
    Task::__tasks.clear();

    for ( std::set<Processor *>::const_iterator p = Processor::__processors.begin(); p != Processor::__processors.end(); ++p ) {
	delete *p;
    }
    Processor::__processors.clear();

    for ( std::set<Entry *>::const_iterator e = Entry::__entries.begin(); e != Entry::__entries.end(); ++e ) {
	delete *e;
    }
    Entry::__entries.clear();

    for ( std::vector<OpenArrivalSource *>::iterator o = OpenArrivalSource::__source.begin(); o != OpenArrivalSource::__source.end(); ++o ) {
	delete *o;
    }
    OpenArrivalSource::__source.clear();

    for ( std::vector<Group *>::iterator g = Group::__groups.begin(); g != Group::__groups.end(); ++g ) {
	delete *g;
    }
    Group::__groups.clear();

    _layers.clear();
    if ( _key ) {
	delete _key;
    }
    if ( _label ) {
	delete _label;
    }

    __model = 0;
}

/*
 * Scale "model" by s.
 */

Model&
Model::operator*=( const double s )
{
    for_each( _layers.begin(), _layers.end(), ::ExecXY<Layer>( &Layer::scaleBy, s, s ) );
    if ( _key ) {
	_key->scaleBy( s, s );
    }
    for_each( Group::__groups.begin(), Group::__groups.end(), ::ExecXY<Group>( &Group::scaleBy, s, s ) );

    _origin  *= s;
    _extent  *= s;
    _scaling  *= s;

    return *this;
}


Model&
Model::translateScale( const double s ) 
{
    for_each( _layers.begin(), _layers.end(), Exec1<Layer,double>( &Layer::translateY, top() ) );
    if ( _key ) {
	_key->translateY( top() );
    }
    for_each( Group::__groups.begin(), Group::__groups.end(), Exec1<Group,double>( &Group::translateY, top() ) );
    *this *= s;

    return *this;
}


Model&
Model::moveBy( const double dx, const double dy ) 
{
    for_each( _layers.begin(), _layers.end(), ::ExecXY<Layer>( &Layer::moveBy, dx, dy ) );
    if ( _key ) {
	_key->moveBy( dx, dy );
    }
    for_each( Group::__groups.begin(), Group::__groups.end(), ::ExecXY<Group>( &Group::moveBy, dx, dy ) );
    _origin.moveBy( dx, dy );

    return *this;
}



#if HAVE_REGEX_T
/*
 * Add a group to the group list.
 */

void
Model::add_group( const std::string& s )
{
    group << new GroupByRegex( s );
}
#endif


/*
 * Create a group for each and every processor in the model so that
 * that GroupModel::justify() will collect everything up properly.
 */

void
Model::group_by_processor()
{
    for ( std::set<Processor *>::const_iterator processor = Processor::__processors.begin(); processor != Processor::__processors.end(); ++processor ) {
	Group::__groups.push_back( new GroupByProcessor( __model->nLayers(), (*processor) ) );
    }
}


/*
 * Create a group for each and every processor in the model so that
 * that GroupModel::justify() will collect everything up properly.
 */

void
Model::group_by_share()
{
    for ( std::set<Processor *>::const_iterator processor = Processor::__processors.begin(); processor != Processor::__processors.end(); ++processor ) {
	if ( (*processor)->nShares() == 0 ) {
	    Group::__groups.push_back( new GroupByProcessor( __model->nLayers(), (*processor) ) );
	} else {
	    for ( std::set<Share *>::const_iterator share = (*processor)->shares().begin(); share != (*processor)->shares().end(); ++share ) {
		Group::__groups.push_back( new GroupByShareGroup( __model->nLayers(), (*processor), *share ) );
	    }
	    Group::__groups.push_back( new GroupByShareDefault( __model->nLayers(), (*processor) ) );
	}
    }
}


/*
 * Put boxes around submodels.
 */

void
Model::group_by_submodel()
{
    for ( unsigned i = SERVER_LEVEL; i < nLayers(); i += 1 ) {
	std::ostringstream s;
	s << "Submodel " << i;
	Group * aGroup = new GroupSquashed( nLayers(), s.str(), _layers.at(i-1), _layers.at(i) );
	aGroup->format().label().resizeBox().positionLabel();
	Group::__groups.push_back( aGroup );
    }
}

bool
Model::prepare( const LQIO::DOM::Document * document )
{
    /* Reset counters */

    openArrivalCount		= 0;
    forwardingCount		= 0;
    for ( unsigned p = 0; p <= MAX_PHASES; ++p ) {
	rendezvousCount[p]	= 0;
	sendNoReplyCount[p] 	= 0;
	phaseCount[p]		= 0;
    }

    /* We use this to add all calls */
    std::vector<LQIO::DOM::Entry*> allEntries;

    /* -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=- [Step 0: Add Pragmas] */
    const std::map<std::string,std::string>& pragmas = document->getPragmaList();
    for_each( pragmas.begin(), pragmas.end(), create_pragma );

    /* -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=- [Step 1: Add Processors] */

    /* We need to add all of the processors */
    const std::map<std::string,LQIO::DOM::Processor*>& processors = document->getProcessors();
    for_each( processors.begin(), processors.end(), Processor::create );

    /* -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=- [Step 1.5: Add Groups] */

    const std::map<std::string,LQIO::DOM::Group*>& groups = document->getGroups();
    for_each( groups.begin(), groups.end(), Share::create );

    /* -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=- [Step 2: Add Tasks/Entries] */

    /* In the DOM, tasks have entries, but here entries need to go first */
    std::vector<Activity*> activityList;
    const std::map<std::string,LQIO::DOM::Task*>& taskList = document->getTasks();

    /* Add all of the tasks we will be needing */
    for ( std::map<std::string,LQIO::DOM::Task*>::const_iterator nextTask = taskList.begin(); nextTask != taskList.end(); ++nextTask ) {
	LQIO::DOM::Task* task = nextTask->second;

	/* Before we can add a task we have to add all of its entries */

	/* Prepare to iterate over all of the entries */
	std::vector<Entry*> entryCollection;
	std::vector<LQIO::DOM::Entry*>::const_iterator nextEntry;
	std::vector<LQIO::DOM::Entry*> activityEntries;

	/* Add the entries so we can reverse them */
	for ( nextEntry = task->getEntryList().begin(); nextEntry != task->getEntryList().end(); ++nextEntry ) {
	    allEntries.push_back( *nextEntry );		// Save the DOM entry.
	    entryCollection.push_back( Entry::create( *nextEntry ) );
	    if ((*nextEntry)->getStartActivity() != nullptr) {
		activityEntries.push_back(*nextEntry);
	    }
	}

	/* Now we can go ahead and add the task */
	Task* aTask = Task::create(task, entryCollection);

	/* Add activities for the task (all of them) */
	const std::map<std::string,LQIO::DOM::Activity*>& activities = task->getActivities();
	std::map<std::string,LQIO::DOM::Activity*>::const_iterator iter;
	for (iter = activities.begin(); iter != activities.end(); ++iter) {
	    const LQIO::DOM::Activity* activity = iter->second;
	    activityList.push_back(Activity::create(aTask, const_cast<LQIO::DOM::Activity*>(activity)));
	}

	/* Set all the start activities */
	for (nextEntry = activityEntries.begin(); nextEntry != activityEntries.end(); ++nextEntry) {
	    LQIO::DOM::Entry* theDOMEntry = *nextEntry;
	    Entry * anEntry = Entry::find( theDOMEntry->getName() );
	    Activity * anActivity = aTask->findActivity(theDOMEntry->getStartActivity()->getName());
	    anEntry->setStartActivity( anActivity );
	}
    }

    /* -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=- [Step 3: Add Calls/Phase Parameters] */

    /* Add all of the calls for all phases to the system */
    const unsigned entryCount = allEntries.size();
    for (unsigned i = 0; i < entryCount; ++i) {
	LQIO::DOM::Entry* entry = allEntries[i];
	Entry* newEntry = Entry::find(entry->getName());
	if ( newEntry == nullptr ) continue;

	/* Handle open arrivals */

	if ( entry->hasOpenArrivalRate() ) {
	    newEntry->isCalled( OPEN_ARRIVAL_REQUEST );
	}

	/* Go over all of the entry's phases and add the calls */
	for (unsigned p = 1; p <= entry->getMaximumPhase(); ++p) {
	    LQIO::DOM::Phase* phase = entry->getPhase(p);
	    const std::vector<LQIO::DOM::Call*>& originatingCalls = phase->getCalls();
	    std::vector<LQIO::DOM::Call*>::const_iterator iter;

	    /* Add all of the calls to the system */
	    for (iter = originatingCalls.begin(); iter != originatingCalls.end(); ++iter) {
		LQIO::DOM::Call* call = *iter;

		/* Add the call to the system */
		newEntry->addCall( p, call );
	    }

	    /* Set the phase information for the entry */
	    std::string phase_name = entry->getName();
	    phase_name += "_";
	    phase_name += p + '0';
	    phase->setName( phase_name );
	}

	/* Add in all of the P(frwd) calls */
	const std::vector<LQIO::DOM::Call*>& forwarding = entry->getForwarding();
	std::vector<LQIO::DOM::Call*>::const_iterator nextFwd;
	for ( nextFwd = forwarding.begin(); nextFwd != forwarding.end(); ++nextFwd ) {
	    Entry* targetEntry = Entry::find((*nextFwd)->getDestinationEntry()->getName());
	    newEntry->forward(targetEntry, *nextFwd );
	}
    }

    /* -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=- [Step 4: Add Calls/Lists for Activities] */

    /* Go back and add all of the lists and calls now that activities all exist */
    std::vector<Activity*>::iterator actIter;
    for (actIter = activityList.begin(); actIter != activityList.end(); ++actIter) {
	Activity* activity = *actIter;
	activity->add_calls()
	    .add_reply_list()
	    .add_activity_lists();
    }

    /* Use the generated connections list to finish up */
    Activity::complete_activity_connections();

    /* Tell the user that we have finished */
    return true;
}



/*
 * layerize and format the input file.
 */

bool
Model::process()
{
    if ( !generate() ) return false;
    if ( !check() ) return false;

#if defined(REP2FLAT)
    switch ( Flags::print[REPLICATION].value.i ) {
    case REPLICATION_EXPAND: expandModel(); /* Fall through to call removeReplication()! */
    case REPLICATION_REMOVE: removeReplication(); break;
    case REPLICATION_RETURN: returnReplication(); break;
    }
#endif

    /* Simplify model if requested. */

    if ( Flags::print[AGGREGATION].value.i != AGGREGATE_NONE ) {
	for_each( Task::__tasks.begin(), Task::__tasks.end(), ::Exec<Entity>( &Entity::aggregate ) );
    }

    /* Assign all tasks to layers. */
    
    layerize();
    for ( std::vector<Layer>::iterator layer = _layers.begin(); layer != _layers.end(); ++layer ) {
	layer->number(layer - _layers.begin());    /* Assign numbers to layers. */
    }

    /* Simplify to tasks (for queueing models) */

    if ( Flags::print[AGGREGATION].value.i == AGGREGATE_ENTRIES ) {
	for_each( _layers.begin(), _layers.end(), ::Exec<Layer>( &Layer::aggregate ) );
    }

    if ( Flags::print[SUMMARY].value.b || Flags::print_submodels ) {
	printSummary( std::cerr );
    } 

    if ( !Flags::have_results && Flags::print[COLOUR].value.i == COLOUR_RESULTS ) {
	Flags::print[COLOUR].value.i = COLOUR_OFF;
    }

    if ( share_output() ) {
	group_by_share();
    } else if ( processor_output() ) {
	group_by_processor();
#if HAVE_REGEX_T
    } else if ( group.size() ) {
	Model::add_group( ".*" );		    /* Tack on default rule */
#endif
    }

    sort( (compare_func_ptr)(&Entity::compare) );
    if ( Flags::rename_model ) {
	rename();
    } else if ( Flags::squish_names ) {
	squishNames();
    }

    Processor * surrogate_processor = 0;
    Task * surrogate_task = 0;

    const unsigned layer = Flags::print[QUEUEING_MODEL].value.i | Flags::print[SUBMODEL].value.i;
    if ( layer > 0 ) {
 	if ( !selectSubmodel( layer ) ) {
	    std::cerr << LQIO::io_vars.lq_toolname << ": Submodel " << layer << " is too big." << std::endl;
	    return false;
	} else if ( Flags::print[LAYERING].value.i != LAYERING_SRVN ) {
 	    _layers.at(layer).generateSubmodel();
	    if ( Flags::surrogates ) {
		_layers[layer].transmorgrify( _document, surrogate_processor, surrogate_task );
		relayerize(layer-1);
		_layers.at(layer).sort( (compare_func_ptr)(&Entity::compare) ).format( 0 ).justify( Entry::__entries.size() * Flags::entry_width );
	    }
	}
#if HAVE_REGEX_T
    } else if ( Flags::print[INCLUDE_ONLY].value.r && Flags::surrogates ) {

	/* Call transmorgrify on all layers */

	for ( unsigned i = layers.size(); i > 0; --i ) {
	    layers[i-1].generateSubmodel();
	    layers[i-1].transmorgrify( _document, surrogate_processor, surrogate_task );
	    relayerize( i-1 );
	    layers[i].sort( Entity::compare ).format( 0 ).justify( LQIO::io_vars.n_entries * Flags::entry_width );
	}
#endif
    }

    if ( !totalize() ) {
	LQIO::solution_error( ERR_NO_OBJECTS );
	return false;
    }

    if ( graphical_output() ) {
	format();

	if ( Group::__groups.size() == 0 && Flags::print_submodels ) {
	    group_by_submodel();
	}

	/* Compensate for the arcs on the right */

	if ( (queueing_output() || submodel_output()) && Flags::flatten_submodel  ) {
	    format( _layers.at(layer) );
	}

	if ( queueing_output() ) {

	    /* Compensate for chains */

	    const double delta = Flags::icon_height / 5.0 * _layers[layer].nChains();
	    _extent.moveBy( delta, delta );
	    _origin.moveBy( 0, -delta / 2.0 );
	}

	/* Compensate for labels */

	if ( Flags::print_layer_number ) {
	    _extent.moveBy( Flags::print[X_SPACING].value.f + _layers.back().labelWidth() + 5, 0 );
	}

	/* Compensate for processor/group boxes. */

	for ( std::vector<Group *>::iterator group = Group::__groups.begin(); group != Group::__groups.end(); ++group ) {
	    double h = (*group)->y() - _origin.y();
	    if ( h < 0 ) {
		_origin.moveBy( 0, h );
		_extent.moveBy( 0, -h );
	    }
	}

	/* Add the solver information and-or comment */

	if ( Flags::print[SOLVER_INFO].value.b ) {
	    *_label << _document->getResultSolverInformation();
	}
	if ( Flags::print[MODEL_COMMENT].value.b ) {
	    if ( _label->size() > 0 ) _label->newLine();
	    *_label << _document->getModelCommentString();
	}
	if ( _label != nullptr && _label->size() ) {
	    _extent.moveBy( 0, _label->height() );
	    _label->moveTo( _origin.x() + _extent.x()/2 , _extent.y() );
	}
	     

	/* Move the key iff necessary */

    	if ( _key ) {
	    _key->label().moveTo( _origin.x(), _origin.y() );

	    switch ( Flags::print[KEY].value.i ) {
	    case KEY_TOP_LEFT:	    _key->moveBy( 0, _extent.y() - _key->height() ); break;
	    case KEY_TOP_RIGHT:	    _key->moveBy( _extent.x() - _key->width(), _extent.y() - _key->height() ); break;
	    case KEY_BOTTOM_LEFT:   break;
	    case KEY_BOTTOM_RIGHT:  _key->moveBy( _extent.x() - _key->width(), 0 ); break;
	    case KEY_BELOW_LEFT:
		moveBy( 0, _key->height() );
		_origin.moveBy( 0, -_key->height() );
		_key->moveBy( 0, -_key->height() );
		_extent.moveBy( 0, _key->height() );
		break;
	    case KEY_ABOVE_LEFT:
		_key->moveBy( 0, _extent.y() );
		_extent.moveBy( 0, _key->height() );
		break;
	    }
	}

	finalScaleTranslate();
    }

    return true;
}


/*
 * Output the results.
 */

bool
Model::store()
{
#if defined(X11_OUTPUT)
    if ( Flags::print[OUTPUT_FORMAT].value.i == FORMAT_X11 ) {
	/* run the event loop */
	return true;
    }
#endif
    if ( output_output() && !Flags::have_results ) {

	std::cerr << LQIO::io_vars.lq_toolname << ": There are no results to output for " << _inputFileName << std::endl;
	return false;

    } else if ( _outputFileName == "-" ) {

	std::cout.exceptions( std::ios::failbit | std::ios::badbit );
	std::cout << *this;

    } else {

	/* Default mapping */

	std::string directory_name;
	std::string suffix = "";

	if ( hasOutputFileName() && LQIO::Filename::isDirectory( _outputFileName ) > 0 ) {
	    directory_name = _outputFileName;
	}

	if ( _document->getResultInvocationNumber() > 0 ) {
	    suffix = SolverInterface::Solve::customSuffix;
	    if ( directory_name.size() == 0 ) {
		/* We need to create a directory to store output. */
		LQIO::Filename filename( hasOutputFileName() ? _outputFileName : _inputFileName, "d" );		/* Get the base file name */
		directory_name = filename();
		int rc = access( directory_name.c_str(), R_OK|W_OK|X_OK );
		if ( rc < 0 ) {
#if defined(WINNT)
		    rc = mkdir( directory_name.c_str() );
#else
		    rc = mkdir( directory_name.c_str(), S_IRUSR|S_IWUSR|S_IXUSR|S_IRGRP|S_IWGRP|S_IXGRP|S_IWOTH|S_IROTH|S_IXOTH );
#endif
		    if ( rc < 0 ) {
			solution_error( LQIO::ERR_CANT_OPEN_DIRECTORY, directory_name.c_str(), strerror( errno ) );
		    }
		}
	    }
	}

	LQIO::Filename filename;
	std::string extension = getExtension();
	if ( !hasOutputFileName() || directory_name.size() > 0 ) {
	    filename.generate( _inputFileName, extension, directory_name, suffix );
	} else {
	    filename = _outputFileName;
	}

	if ( _inputFileName == filename() && input_output() ) {
#if defined(REP2FLAT)
	    if ( Flags::print[REPLICATION].value.i == REPLICATION_EXPAND ) {
		std::string ext = ".";		// look for .ext
		ext += extension;
		const size_t pos = filename.rfind( ext );
		if ( pos != std::string::npos ) {
		    filename.insert( pos, "-flat" );	// change filename.ext to filename-flat.ext
		} else {
		    std::ostringstream msg;
		    msg << "Cannot overwrite input file " << filename() << " with a subset of original model.";
		    throw std::runtime_error( msg.str() );
		}
	    } else if ( partial_output()
			|| Flags::print[AGGREGATION].value.i != AGGREGATE_NONE
			|| Flags::print[REPLICATION].value.i != REPLICATION_NOP ) {
		std::ostringstream msg;
		msg << "Cannot overwrite input file " << filename() << " with a subset of original model.";
		throw std::runtime_error( msg.str() );
	    }
#else
	    if ( partial_output()
		 || Flags::print[AGGREGATION].value.i != AGGREGATE_NONE ) {
		ostringstream msg;
		msg << "Cannot overwrite input file " << filename() << " with a subset of original model.";
		throw runtime_error( msg.str() );
	    }
#endif
	}

	filename.backup();

	std::ofstream output;
	switch( Flags::print[OUTPUT_FORMAT].value.i ) {
#if defined(EMF_OUTPUT)
	case FORMAT_EMF:
	    if ( LQIO::Filename::isRegularFile( filename() ) == 0 ) {
		std::ostringstream msg;
		msg << "Cannot open output file " << filename() << " - not a regular file.";
		throw std::runtime_error( msg.str() );
	    } else {
		output.open( filename().c_str(), std::ios::out|std::ios::binary );	/* NO \r's in output for windoze */
	    }
	    break;
#endif
#if HAVE_GD_H && HAVE_LIBGD
#if HAVE_GDIMAGEGIFPTR
	case FORMAT_GIF:
#endif
#if HAVE_LIBJPEG
	case FORMAT_JPEG:
#endif
#if HAVE_LIBPNG
	case FORMAT_PNG:
#endif
	    output.open( filename().c_str(), std::ios::out|std::ios::binary );	/* NO \r's in output for windoze */
	    break;
#endif /* HAVE_LIBGD */

	default:
	    output.open( filename().c_str(), std::ios::out );
	    break;
	}

	if ( !output ) {
	    std::ostringstream msg;
	    msg << "Cannot open output file " << filename() << " - " << strerror( errno );
	    throw std::runtime_error( msg.str() );
	} else {
	    output.exceptions ( std::ios::failbit | std::ios::badbit );
	    output << *this;
	}
	output.close();
    }
    return true;
}



/*
 * Output the results.
 */

bool
Model::reload()
{
    /* Default mapping */

    LQIO::Filename directory_name( hasOutputFileName() ? _outputFileName : _inputFileName, "d" );		/* Get the base file name */

    if ( access( directory_name().c_str(), R_OK|X_OK ) < 0 ) {
	solution_error( LQIO::ERR_CANT_OPEN_DIRECTORY, directory_name().c_str(), strerror( errno ) );
	throw LQX::RuntimeException( "--reload-lqx can't load results." );
    }

    unsigned int errorCode;
    if ( !_document->loadResults( directory_name(), _inputFileName,
				  SolverInterface::Solve::customSuffix, errorCode ) ) {
	throw LQX::RuntimeException( "--reload-lqx can't load results." );
    } else {
	label();
	store();
	return true;
    }
}



std::string
Model::getExtension()
{
    std::string extension;

    switch ( Flags::print[OUTPUT_FORMAT].value.i ) {
    case FORMAT_EEPIC:
	extension = "tex";
	break;
#if defined(EMF_OUTPUT)
    case FORMAT_EMF:
	extension = "emf";
	break;
#endif
    case FORMAT_PSTEX:
    case FORMAT_FIG:
	extension = "fig";
	break;
#if HAVE_GD_H && HAVE_LIBGD
#if HAVE_GDIMAGEGIFPTR
    case FORMAT_GIF:
	extension = "gif";
	break;
#endif
#if HAVE_LIBJPEG
    case FORMAT_JPEG:
	extension = "jpg";
	break;
#endif
#if HAVE_LIBPNG
    case FORMAT_PNG:
	extension = "png";
	break;
#endif
#endif	/* HAVE_LIBGD */
    case FORMAT_NULL:
	extension = "txt";
	break;
    case FORMAT_OUTPUT:
	extension = "out";
	break;
    case FORMAT_PARSEABLE:
	extension = "p";
	break;
    case FORMAT_POSTSCRIPT:
	extension = "ps";
	break;
    case FORMAT_RTF:
	extension = "rtf";
	break;
    case FORMAT_SRVN:
	extension = "lqn";
	{
	    std::size_t i = _inputFileName.find_last_of( '.' );
	    if ( i != std::string::npos ) {
		std::string ext = _inputFileName.substr( i+1 );
		std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
		if ( ext != "lqnx" && ext != "xlqn" && ext != "xml" && ext != "lqxo" ) {
		    extension = ext;
		}
	    }
	}
	break;
#if defined(SVG_OUTPUT)
    case FORMAT_SVG:
	extension = "svg";
	break;
#endif
#if defined(SXD_OUTPUT)
    case FORMAT_SXD:
	abort();
	break;
#endif
#if defined(TXT_OUTPUT)
    case FORMAT_TXT:
	extension = "txt";
	break;
#endif
    case FORMAT_LQX:
    case FORMAT_XML:
	extension = "lqnx";
	break;
    default:
	abort();
	break;
    }
    return extension;
}


/*
 * Sort tasks into layers.
 */

bool
Model::generate()
{
    /* At this point we need to do some intelligent thing about sticking tasks/processors into submodels. */

    for ( std::set<Task *>::const_iterator task = Task::__tasks.begin(); task != Task::__tasks.end(); ++task ) {
	if ( !(*task)->isReachable() ) {
	    LQIO::solution_error( LQIO::WRN_NOT_USED, "Task", (*task)->name().c_str() );
	} else if ( (*task)->hasActivities() ) {
	    (*task)->generate();
	}
    }

    return Flags::print[IGNORE_ERRORS].value.b || !LQIO::io_vars.anError();
}



/*
 * Sort tasks into layers.  Start from reference tasks only and tasks
 * with open arrivals.  If a task has open arrivals, start from level
 * 1 so that it is treated as a server.
 */

unsigned
Model::topologicalSort()
{
    size_t max_depth = 0;

    unsigned int i = 1;			/* Client path number */
    for ( std::set<Task *>::const_iterator task = Task::__tasks.begin(); task != Task::__tasks.end(); ++task ) {
	if ( (*task)->rootLevel() == Task::IS_NON_REFERENCE 

#if HAVE_REGEX_T
	     || (Flags::client_tasks != nullptr && regexec( Flags::client_tasks, const_cast<char *>((*task)->name().c_str()), 0, 0, 0 ) == REG_NOMATCH )
#endif
	    ) continue;

	try {
	    CallStack callStack;
	    if ( (*task)->rootLevel() == Task::HAS_OPEN_ARRIVALS ) callStack.push_back( nullptr );
	    max_depth = std::max( (*task)->findChildren( callStack, i ), max_depth );
	}
	catch( const Call::cycle_error& error ) {
	    max_depth = std::max( error.depth(), max_depth );
	    LQIO::solution_error( LQIO::ERR_CYCLE_IN_CALL_GRAPH, error.what() );
	}

	i += 1;
    }

    return max_depth;
}



/*
 * Select a submodel.
 */

bool
Model::selectSubmodel( const unsigned submodel )
{
    if ( submodel == 0 || submodel >= nLayers() ) {
	return false;
    } else {
	_layers.at(submodel).selectSubmodel();
	return true;
    }
}


Model&
Model::relayerize( const unsigned level )
{
    std::vector<Entity *>& entities = const_cast<std::vector<Entity *>& >(_layers.at(level).entities());
    for ( std::vector<Entity *>::iterator entity = entities.begin(); entity != entities.end(); ++entity ) {
	if ( (*entity)->level() > level ) {
	    _layers[level].erase(entity);
	    _layers[level+1].append(*entity);
	}
    }
    return *this;
}

/*
 *
 */

bool
Model::check() const
{
    for_each( Processor::__processors.begin(), Processor::__processors.end(), Predicate<Entity>( &Entity::check ) );
    for_each( Task::__tasks.begin(), Task::__tasks.end(), Predicate<Entity>( &Entity::check ) );
    return !LQIO::io_vars.anError();
}



/*
 * Now count them up.
 */

unsigned
Model::totalize()
{
    _total = 0;
    for ( std::vector<Layer>::const_iterator layer = _layers.begin(); layer != _layers.end(); ++layer ) {
	_total += for_each( layer->entities().begin(), layer->entities().end(), Count() );
    }
    return _total.tasks() + _total.processors();
}


/*
 * Order tasks intelligently (!) in each layer. Start at the top.
 */

Model&
Model::sort( compare_func_ptr compare )
{
    const double width = Entry::__entries.size() * Flags::entry_width;		/* A guess... */
    for ( std::vector<Layer>::iterator layer = _layers.begin(); layer != _layers.end(); ++layer ) {
	if ( !*layer == 0 ) continue;
	layer->sort( compare ).format( 0 ).justify( width );
    }
    return *this;
}



/*
 * Rename all objects.  Objects have been sorted, so everything will look real perty.
 */

Model&
Model::rename()
{
    for_each( Processor::__processors.begin(), Processor::__processors.end(), ::Exec<Element>( &Element::rename ) );
//    for_each( Group::__groups.begin(), Group::__groups.end(), ::Exec<Element>( &Element::rename ) );
    for_each( Task::__tasks.begin(), Task::__tasks.end(), ::Exec<Element>( &Element::rename ) );
    for_each( Entry::__entries.begin(), Entry::__entries.end(), ::Exec<Element>( &Element::rename ) );
    return *this;
}



/*
 * Rename all objects.  Objects have been sorted, so everthing will look real perty.
 */

Model&
Model::squishNames()
{
    for_each( Processor::__processors.begin(), Processor::__processors.end(), ::Exec<Element>( &Element::squishName ) );
    for_each( Task::__tasks.begin(), Task::__tasks.end(), ::Exec<Element>( &Element::squishName ) );
    for_each( Entry::__entries.begin(), Entry::__entries.end(), ::Exec<Element>( &Element::squishName ) );
    return *this;
}



/*
 * Format for printing.  Start from the bottom.
 */

Model&
Model::format()
{
    _origin.moveTo( 0., 0. );
    _extent.moveTo( 0., 0. );

    double start_y = 0.0;

    for ( std::vector<Layer>::reverse_iterator layer = _layers.rbegin(); layer != _layers.rend(); ++layer ) {
	if ( !*layer ) continue;
	layer->format( start_y ).label().sort( (compare_func_ptr)(&Entity::compare) ).depth( (nLayers() - layer->number()) * 10 );
	_origin.min( layer->x(), layer->y() );
	_extent.max( layer->x() + layer->width(), layer->y() + layer->height() );

	start_y += (layer->height() + Flags::print[Y_SPACING].value.f);
    }

    justify();
    align();

    switch ( Flags::node_justification ) {
    case DEFAULT_JUSTIFY:
    case ALIGN_JUSTIFY:
	if ( Flags::print[LAYERING].value.i == LAYERING_BATCH
	     || Flags::print[LAYERING].value.i == LAYERING_HWSW
	     || Flags::print[LAYERING].value.i == LAYERING_MOL ) {
	    alignEntities();
	}
	break;
    }

    for ( std::vector<Layer>::reverse_iterator layer = _layers.rbegin(); layer != _layers.rend(); ++layer ) {
	if ( !*layer ) continue;
	layer->moveLabelTo( right() + Flags::print[X_SPACING].value.f, layer->height() / 2.0 );
    }
    return *this;
}


const Model&
Model::format( Layer& serverLayer )
{
    Layer clientLayer;
    for ( std::vector<Entity *>::const_iterator client = serverLayer.clients().begin(); client != serverLayer.clients().end(); ++client ) {
	clientLayer.append( const_cast<Entity *>(*client) );
    }

    double start_y = serverLayer.y();
    _origin.moveTo( MAXDOUBLE, MAXDOUBLE );
    _extent.moveTo( 0, 0 );

    serverLayer.format( start_y ).sort( (compare_func_ptr)(Entity::compareCoord) );
    _origin.min( serverLayer.x(), serverLayer.y() );
    _extent.max( serverLayer.x() + serverLayer.width(), serverLayer.y() + serverLayer.height() );
    start_y += ( serverLayer.x() + serverLayer.height() + Flags::print[Y_SPACING].value.f);

    clientLayer.format( start_y ).sort( (compare_func_ptr)(&Entity::compareCoord) );
    _origin.min( clientLayer.x(), clientLayer.y() );
    _extent.max( clientLayer.x() + clientLayer.width(), clientLayer.y() + clientLayer.height() );

    clientLayer.justify( right() );
    serverLayer.justify( right() );

    return *this;
}


/*
 * Justify the layers.
 */

Model&
Model::justify()
{
    for_each( _layers.begin(), _layers.end(), Exec1<Layer,double>( &Layer::justify, right() ) );
    return *this;
}



/*
 * Align the layers horizontally.
 */

Model&
Model::align()
{
    for_each( _layers.begin(), _layers.end(), ::Exec<Layer>( &Layer::align ) );
    return *this;
}



/*
 * Align tasks between layers.
 */

Model&
Model::alignEntities()
{
    if ( _layers.size() < 2 ) return *this;	/* No point */

    double maxRight = 0.0;
    for ( std::vector<Layer>::iterator layer = _layers.begin(); layer != _layers.end(); ++layer ) {
	if ( !*layer ) {
	    continue;
	} else if ( layer->number() == CLIENT_LEVEL ) {
	    layer->fill( right() );
	} else {
	    layer->alignEntities();
	}
	maxRight = std::max( maxRight, layer->x() + layer->width() );
    }

    _origin.x( 0 );
    _extent.x( maxRight );
    return *this;
}



/*
 * Our origin is lower left corner (Like TeX).  Most everything else
 * is upper right.  Scale to final image size so that the output is
 * roughly the same, regarless of format.
 */

Model&
Model::finalScaleTranslate()
{
    /*
     * Shift everything to origin.  Makes life easier for EMF and
     * other formats with clipping rectangles. Add a border.
     */

    const double offset = Flags::print[BORDER].value.f;
    const double x_offset = offset - left();
    const double y_offset = offset - bottom();		/* Shift to origin */
    _origin.moveTo( 0, 0 );
    for_each( _layers.begin(), _layers.end(), ::ExecXY<Layer>( &Layer::moveBy, x_offset, y_offset ) );
    if ( _key ) {
	_key->moveBy( x_offset, y_offset );
    }
    if ( _label ) {
	_label->moveBy( x_offset, y_offset );
    }
    for_each( Group::__groups.begin(), Group::__groups.end(), ::ExecXY<Group>( &Group::moveBy, x_offset, y_offset ) );
    _extent.moveBy( 2.0 * offset, 2.0 * offset );

    /* Rescale for output format. */

    switch ( Flags::print[OUTPUT_FORMAT].value.i ) {
    case FORMAT_EEPIC:
	*this *= EEPIC_SCALING;
	break;

#if defined(EMF_OUTPUT)
    case FORMAT_EMF:
	translateScale( EMF_SCALING );
	break;
#endif

    case FORMAT_FIG:
    case FORMAT_PSTEX:
	translateScale( FIG_SCALING );
	break;

#if HAVE_GD_H && HAVE_LIBGD
#if HAVE_GDIMAGEGIFPTR
    case FORMAT_GIF:
#endif
#if HAVE_LIBJPEG
    case FORMAT_JPEG:
#endif
#if HAVE_LIBPNG
    case FORMAT_PNG:
#endif
	translateScale( GD_SCALING );
	break;
#endif	/* HAVE_LIBGD */

#if defined(SVG_OUTPUT)
    case FORMAT_SVG:
	/* TeX's origin is lower left corner.  SVG's is upper right.  Fix and scale */
	translateScale( SVG_SCALING );
	break;
#endif

#if defined(SXD_OUTPUT)
    case FORMAT_SXD:
	/* TeX's origin is lower left corner.  SXD's is upper right.  Fix and scale */
	translateScale( SXD_SCALING );
	break;
#endif

    }

    /* Final scaling */

    if ( Flags::print[MAGNIFICATION].value.f != 1.0 ) {
	*this *= Flags::print[MAGNIFICATION].value.f;
    }

    return *this;
}



Model&
Model::label()
{
    Flags::have_results = Flags::print[RESULTS].value.b && ( _document->getResultIterations() > 0 || _document->getResultConvergenceValue() > 0 );
    for_each( _layers.rbegin(), _layers.rend(), Exec<Layer>( &Layer::label ) );
    return *this;
}


unsigned
Model::count( const taskPredicate aFunc ) const
{
    return for_each( _layers.begin(), _layers.end(), ::Count<Layer,taskPredicate>( &Layer::count, aFunc ) ).count();
}



unsigned
Model::count( const callPredicate aFunc ) const
{
    return for_each( _layers.begin(), _layers.end(), ::Count<Layer,callPredicate>( &Layer::count, aFunc ) ).count();
}



unsigned
Model::nMultiServers() const
{
    return count( &Task::isMultiServer );
}


unsigned
Model::nInfiniteServers() const
{
    return count( &Task::isInfinite );
}


Model&
Model::accumulateStatistics( const std::string& filename )
{
    stats[TOTAL_LAYERS].accumulate( this, filename );
    stats[TOTAL_TASKS].accumulate( this, filename );
    stats[TOTAL_PROCESSORS].accumulate( this, filename );
    stats[TOTAL_ENTRIES].accumulate( this, filename );
    stats[TOTAL_INFINITE_SERVERS].accumulate( this, filename );
    stats[TOTAL_MULTISERVERS].accumulate( this, filename );

    accumulateEntryStats( filename );
    accumulateTaskStats( filename );
    return *this;
}



const Model&
Model::accumulateTaskStats( const std::string& filename ) const
{
    /* Does not count ref. tasks. */

    for ( std::vector<Layer>::const_iterator layer = _layers.begin(); layer != _layers.end(); ++layer ) {
	if ( !*layer ) continue;
	unsigned nTasks = count_if( layer->entities().begin(), layer->entities().end(), Predicate<Entity>( &Entity::isServerTask ) );
	if ( nTasks ) {
	    stats[TASKS_PER_LAYER].accumulate( nTasks, filename );
	}
    }
    return *this;
}




const Model&
Model::accumulateEntryStats( const std::string& filename ) const
{
    for ( std::vector<Layer>::const_iterator layer = _layers.begin(); layer != _layers.end(); ++layer ) {
	for ( std::vector<Entity *>::const_iterator entity = layer->entities().begin(); entity != layer->entities().end(); ++entity ) {
	    const Task * aTask = dynamic_cast<const Task *>(*entity);

	    /* Does not count ref. tasks. */

	    if ( aTask && aTask->isServerTask() ) {
		stats[ENTRIES_PER_TASK].accumulate( aTask->nEntries(), filename );
		for ( std::vector<Entry *>::const_iterator entry = aTask->entries().begin(); entry != aTask->entries().end(); ++entry ) {
		    if ( (*entry)->hasOpenArrivalRate() ) {
			stats[OPEN_ARRIVALS_PER_ENTRY].accumulate( 1.0, filename );
			stats[OPEN_ARRIVAL_RATE_PER_ENTRY].accumulate( LQIO::DOM::to_double((*entry)->openArrivalRate()), filename );
		    } else {
			stats[OPEN_ARRIVALS_PER_ENTRY].accumulate( 0.0, filename );
		    }
		    if ( (*entry)->hasForwarding() ) {
			stats[FORWARDING_PER_ENTRY].accumulate( 1.0, filename );
		    } else {
			stats[FORWARDING_PER_ENTRY].accumulate( 0.0, filename );
		    }
		    stats[RNVS_PER_ENTRY].accumulate( (*entry)->countCallers( &GenericCall::hasRendezvous ), filename );
		    stats[SNRS_PER_ENTRY].accumulate( (*entry)->countCallers( &GenericCall::hasSendNoReply ), filename );
		    stats[PHASES_PER_ENTRY].accumulate( (*entry)->maxPhase(), filename );
		    for ( unsigned p = 1; p <= (*entry)->maxPhase(); ++p ) {
			if ( (*entry)->hasServiceTime(p) ) {
			    stats[SERVICE_TIME_PER_PHASE].accumulate( LQIO::DOM::to_double((*entry)->serviceTime( p )), filename );
			}
		    }

		    /* get rendezvous rates and what have you */

		    for ( std::vector<Call *>::const_iterator call = (*entry)->calls().begin(); call != (*entry)->calls().end(); ++call ) {
			for ( unsigned p = 1; p <= (*entry)->maxPhase(); ++p ) {
			    if ( (*call)->hasRendezvousForPhase(p) ) {
				stats[RNV_RATE_PER_CALL].accumulate( LQIO::DOM::to_double((*call)->rendezvous(p)), filename );
			    }
			    if ( (*call)->hasSendNoReplyForPhase(p) ) {
				stats[RNV_RATE_PER_CALL].accumulate( LQIO::DOM::to_double((*call)->sendNoReply(p)), filename );
			    }
			}
			if ( (*call)->hasForwarding() ) {
			    stats[FORWARDING_PROBABILITY_PER_CALL].accumulate( LQIO::DOM::to_double((*call)->forward()), filename );
			}
		    }
		}

		const std::vector<Activity *> activities = aTask->activities();
		for ( std::vector<Activity *>::const_iterator activity = activities.begin(); activity != activities.end(); ++activity ) {
		    if ( (*activity)->hasServiceTime() ) {
			stats[SERVICE_TIME_PER_PHASE].accumulate( LQIO::DOM::to_double((*activity)->serviceTime()), filename );
		    }
		}
	    }
	}
    }
    return *this;
}



/*
 * Print out the model.
 */

std::ostream&
Model::print( std::ostream& output ) const
{
    if ( Flags::print_comment && Flags::print[OUTPUT_FORMAT].value.i != FORMAT_TXT) {
	const char * comment = getDOM()->getModelCommentString();
	if ( comment && comment[0] != '\0' ) {
	    std::cout << _inputFileName << ": " << comment << std::endl;
	}
    }

    switch( Flags::print[OUTPUT_FORMAT].value.i ) {
    case FORMAT_EEPIC:
	printEEPIC( output );
	break;
#if defined(EMF_OUTPUT)
    case FORMAT_EMF:
	printEMF( output );
	break;
#endif
    case FORMAT_FIG:
    case FORMAT_PSTEX:
	printFIG( output );
	break;
#if HAVE_GD_H && HAVE_LIBGD && HAVE_GDIMAGEGIFPTR
    case FORMAT_GIF:
	printGD( output, &GD::outputGIF );
	break;
#endif
#if HAVE_GD_H && HAVE_LIBGD && HAVE_LIBJPEG
    case FORMAT_JPEG:
	printGD( output, &GD::outputJPG );
	break;
#endif
    case FORMAT_NULL:
	break;
#if HAVE_GD_H && HAVE_LIBGD && HAVE_LIBPNG
    case FORMAT_PNG:
	printGD( output, &GD::outputPNG );
	break;
#endif
    case FORMAT_OUTPUT:
	if ( Flags::print[PRECISION].value.i >= 0 ) {
	    output.precision(Flags::print[PRECISION].value.i);
	}
	printOutput( output );
	break;
    case FORMAT_PARSEABLE:
	printParseable( output );
	break;
    case FORMAT_POSTSCRIPT:
	printPostScript( output );
	break;
    case FORMAT_RTF:
	printRTF( output );
	break;
#if defined(SVG_OUTPUT)
    case FORMAT_SVG:
	printSVG( output );
	break;
#endif
#if defined(SXD_OUTPUT)
    case FORMAT_SXD:
	printSXD( output );
	break;
#endif
#if defined(TXT_OUTPUT)
    case FORMAT_TXT:
	printTXT( output );
	break;
#endif
    case FORMAT_SRVN:
	printInput( output  );
	break;
    case FORMAT_LQX:
	LQIO::Spex::clear();		/* removes spex, so LQX will output. */
    case FORMAT_XML:
	printXML( output );
	break;
#if defined(X11_OUTPUT)
    case FORMAT_X11:
	printX11( output );
	break;
#endif
    }

    return output;
}

/* ------------------------------------------------------------------------ */
/*                      Major model transmorgrification                     */
/* ------------------------------------------------------------------------ */

#if defined(REP2FLAT)
Model&
Model::expandModel()
{
    /* Copy arrays */

    std::set<Processor *,LT<Processor> > old_processor( Processor::__processors );
    std::set<Task *,LT<Task> > old_task( Task::__tasks );
    std::set<Entry *,LT<Entry> > old_entry( Entry::__entries );

    /* Reset old arrays for new entries. */

    Entry::__entries.clear();
    Task::__tasks.clear();
    Processor::__processors.clear();
    _document->clearAllMaps();

    /* Expand Processors and entries */

    for_each( old_processor.begin(), old_processor.end(), Exec<Processor>( &Processor::expandProcessor ) );
    for_each( old_entry.begin(), old_entry.end(), Exec<Entry>( &Entry::expandEntry ) );
    for_each( old_task.begin(), old_task.end(), Exec<Task>( &Task::expandTask ) );
    for_each( old_entry.begin(), old_entry.end(), Exec<Entry>( &Entry::expandCall ) );

    /*  Delete all original Entities from the symbol table and collections */

    for ( std::set<Entry *>::const_iterator entry = old_entry.begin(); entry != old_entry.end(); ++entry ) {
	delete *entry;
    }
    for ( std::set<Task *>::const_iterator task = old_task.begin(); task != old_task.end(); ++task ) {
	delete *task;
    }
    for ( std::set<Processor *>::const_iterator processor = old_processor.begin(); processor != old_processor.end(); ++processor ) {
	delete *processor;
    }

    return *this;
}



Model&
Model::removeReplication()
{
    for_each( Task::__tasks.begin(), Task::__tasks.end(), Exec<Entity>( &Entity::removeReplication ) );
    for_each( Processor::__processors.begin(), Processor::__processors.end(), Exec<Entity>( &Entity::removeReplication ) );
    return *this;
}



/*
 * Revert a flattened model to a replicated model.
 */

Model&
Model::returnReplication()
{
    /* Copy arrays */

    std::set<Processor *,LT<Processor> > old_processor( Processor::__processors );
    std::set<Task *,LT<Task> > old_task( Task::__tasks );
    std::set<Entry *,LT<Entry> > old_entry( Entry::__entries );

    /* Reset old arrays for new entries. */

    Entry::__entries.clear();
    Task::__tasks.clear();
    Processor::__processors.clear();
    _document->clearAllMaps();

    LQIO::DOM::DocumentObject * root = nullptr;
    for_each( old_processor.begin(), old_processor.end(), Exec1<Processor,LQIO::DOM::DocumentObject **>( &Processor::replicateProcessor, &root ) );
    for_each( old_entry.begin(), old_entry.end(), Exec<Entry>( &Entry::replicateCall ) );	/* do before entry */
    for_each( old_task.begin(), old_task.end(), Exec<Task>( &Task::replicateCall ) );		/* do before task */
    for_each( old_entry.begin(), old_entry.end(), Exec1<Entry,LQIO::DOM::DocumentObject **>( &Entry::replicateEntry, &root ) );
    for_each( old_task.begin(), old_task.end(), Exec1<Task,LQIO::DOM::DocumentObject **>( &Task::replicateTask, &root ) );
    Task::updateFanInOut();
    return *this;
}
#endif

/* ------------------------ Graphical output. ------------------------- */

/*
 * Print all the layers.
 */

std::ostream&
Model::printEEPIC( std::ostream & output ) const
{
    output << "% Created By: " << LQIO::io_vars.lq_toolname << " Version " << VERSION << std::endl
	   << "% Invoked as: " << command_line << ' ' << _inputFileName << std::endl
	   << "\\setlength{\\unitlength}{" << 1.0/EEPIC_SCALING << "pt}" << std::endl
	   << "\\begin{picture}("
	   << static_cast<int>(right()+0.5) << "," << static_cast<int>(top() + 0.5)
	   << ")(" << static_cast<int>(bottom()+0.5)
	   << "," << -static_cast<int>(bottom()+0.5) << ")" << std::endl;

    printLayers( output );

    output << "\\end{picture}" << std::endl;
    return output;
}


#if defined(EMF_OUTPUT)
std::ostream&
Model::printEMF( std::ostream& output ) const
{
    /* header start */
    EMF::init( output, right(), top(), command_line );
    printLayers( output );
    EMF::terminate( output );
    return output;
}
#endif


/*
 * See http://www.xfig.org/userman/fig-format.html
 */

std::ostream&
Model::printFIG( std::ostream& output ) const
{
    output << "#FIG 3.2" << std::endl
	   << "Portrait" << std::endl
	   << "Center" << std::endl
	   << "Inches" << std::endl
	   << "Letter" << std::endl
	   << "75.00" << std::endl
	   << "Single" << std::endl
	   << "-2" << std::endl;
    output << "# Created By: " << LQIO::io_vars.lq_toolname << " Version " << VERSION << std::endl
	   << "# Invoked as: " << command_line << ' ' << _inputFileName << std::endl
	   << "# " << LQIO::DOM::Common_IO::svn_id() << std::endl
	   << print_comment( "# ", *getDOM()->getModelComment() ) << std::endl;
    output << "1200 2" << std::endl;
    Fig::initColours( output );

    /* alignment markers */

    if ( (submodel_output()
	 || queueing_output()
	 || Flags::print[CHAIN].value.i)
	&& Flags::print_alignment_box ) {
	Fig alignment;
	std::vector<Point> points(4);
	points[0].moveTo( left(), bottom() );
	points[1].moveTo( left(), top() );
	points[2].moveTo( right(), top() );
	points[3].moveTo( right(), bottom() );
	alignment.polyline( output, points, Fig::POLYGON, Graphic::WHITE, Graphic::TRANSPARENT, (_layers.size()+1)*10 );
    }

    printLayers( output );

    return output;
}


#if HAVE_GD_H && HAVE_LIBGD
std::ostream&
Model::printGD( std::ostream& output, outputFuncPtr func ) const
{
    GD::create( static_cast<int>(right()+0.5), static_cast<int>(top()+0.5) );
    printLayers( output );
    (* func)( output );
    GD::destroy();
    return output;
}
#endif


std::ostream&
Model::printPostScript( std::ostream& output ) const
{
    printPostScriptPrologue( output, _inputFileName,
			     static_cast<int>(left()+0.5),
			     static_cast<int>(bottom()+0.5),
			     static_cast<int>(right()+0.5),
			     static_cast<int>(top()+0.5) );

    output << "save" << std::endl;

    PostScript::init( output );

    printLayers( output );

    output << "showpage" << std::endl;
    output << "restore" << std::endl;
    return output;
}



#if defined(SVG_OUTPUT)
std::ostream&
Model::printSVG( std::ostream& output ) const
{
    output << "<?xml version=\"1.0\" standalone=\"no\"?>" << std::endl
	   << "<!DOCTYPE svg PUBLIC \"-//W3C//DTD SVG 20000303 Stylable//EN\"" << std::endl
	   << "\"http://www.w3.org/TR/2000/03/WD-SVG-20000303/DTD/svg-20000303-stylable.dtd\">" << std::endl;
    output << "<!-- Title: " << _inputFileName << " -->" << std::endl;
    output << "<!-- Creator: " << LQIO::io_vars.lq_toolname << " Version " << VERSION << " -->" << std::endl;
#if defined(HAVE_CTIME)
    output << "<!-- ";
    time_t clock = time( (time_t *)0 );
    for ( char * s = ctime( &clock ); *s && *s != '\n'; ++s ) {
	output << *s;
    }
    output << " -->" << std::endl;
#endif
    output << "<!-- For: " << _login << " -->" << std::endl;
    output << "<!-- Invoked as: " << command_line << ' ' << _inputFileName << " -->" << std::endl;
    output << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\""
	   << right() / (SVG_SCALING * 72.0) << "in\" height=\""
	   << top() / (SVG_SCALING * 72.0) << "in\" viewBox=\""
	   << "0 0 "
	   << static_cast<int>(right() + 0.5)<< " "
	   << static_cast<int>(top() + 0.5) << "\">" << std::endl;
    output << "<desc>" << getDOM()->getModelComment() << "</desc>" << std::endl;

    printLayers( output );
    output << "</svg>" << std::endl;
    return output;
}
#endif

#if defined(SXD_OUTPUT)
/*
 * This guy is complicated...
 * We dump to contents.xml, then zip a bunch of crap together.
 * Yow!
 */

std::ostream&
Model::printSXD( std::ostream& output ) const
{
    output << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>" << std::endl
	   << "<!DOCTYPE office:document-content PUBLIC \"-//OpenOffice.org//DTD OfficeDocument 1.0//EN\" \"office.dtd\">" << std::endl;

    set_indent(0);
    output << indent( +1 ) << "<office:document-content xmlns:office=\"http://openoffice.org/2000/office\" xmlns:style=\"http://openoffice.org/2000/style\" xmlns:text=\"http://openoffice.org/2000/text\" xmlns:table=\"http://openoffice.org/2000/table\" xmlns:draw=\"http://openoffice.org/2000/drawing\" xmlns:fo=\"http://www.w3.org/1999/XSL/Format\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" xmlns:number=\"http://openoffice.org/2000/datastyle\" xmlns:presentation=\"http://openoffice.org/2000/presentation\" xmlns:svg=\"http://www.w3.org/2000/svg\" xmlns:chart=\"http://openoffice.org/2000/chart\" xmlns:dr3d=\"http://openoffice.org/2000/dr3d\" xmlns:math=\"http://www.w3.org/1998/Math/MathML\" xmlns:form=\"http://openoffice.org/2000/form\" xmlns:script=\"http://openoffice.org/2000/script\" office:class=\"drawing\" office:version=\"1.0\">" << std::endl;
    output << indent( 0 )  << "<office:script/>" << std::endl;

    SXD::init( output );

    output << indent( +1 ) << "<office:body>" << std::endl;
    output << indent( +1 ) << "<draw:page draw:name=\""
	   << _inputFileName
	   << "\" draw:style-name=\"dp1\" draw:master-page-name=\"Default\">" << std::endl;

    printLayers( output );

    output << indent( -1 ) << "</draw:page>" << std::endl;
    output << indent( -1 ) << "</office:body>" << std::endl;
    output << indent( -1 ) << "</office:document-content>" << std::endl;

    return output;
}
#endif
#if defined(X11_OUTPUT)
std::ostream&
Model::printX11( std::ostream& output ) const
{
    return output;
}
#endif

/* ---------------------- Non-graphical output. ----------------------- */


/*
 * Change the order of the entity list from the order of input to the order we have assigned.
 */

std::map<unsigned, LQIO::DOM::Entity *>&
Model::remapEntities() const
{
    std::map<unsigned, LQIO::DOM::Entity *>& entities = const_cast<std::map<unsigned, LQIO::DOM::Entity *>&>(getDOM()->getEntities());
    entities.clear();
    for_each( _layers.begin(), _layers.end(), Remap( entities ) );
    return entities;
}

void
Model::Remap::operator()( const Layer& layer )
{
    for_each( layer.entities().begin(), layer.entities().end(), Remap( _entities ) );
}

void
Model::Remap::operator()( const Entity * entity )
{
    const unsigned int i = _entities.size() + 1;
    _entities[i] = const_cast<LQIO::DOM::Entity *>(dynamic_cast<const LQIO::DOM::Entity *>(entity->getDOM()));	/* Our order, not the dom's */
}


/*
 * Output an input file.
 */

std::ostream&
Model::printInput( std::ostream& output ) const
{
    LQIO::SRVN::Input srvn( *getDOM(), remapEntities(), Flags::annotate_input );
    srvn.print( output );
    return output;
}


/*
 * Output an output file.
 */

std::ostream&
Model::printOutput( std::ostream& output ) const
{
    LQIO::SRVN::Output srvn( *getDOM(), remapEntities(), Flags::print[CONFIDENCE_INTERVALS].value.b, Flags::print[VARIANCE].value.b, Flags::print[HISTOGRAMS].value.b );
    srvn.print( output );
    return output;
}



/*
 * Output an output file.
 */

std::ostream&
Model::printParseable( std::ostream& output ) const
{
    LQIO::SRVN::Parseable srvn( *getDOM(), remapEntities(), Flags::print[CONFIDENCE_INTERVALS].value.b );
    srvn.print( output );
    return output;
}



/*
 * Output an output file.
 */

std::ostream&
Model::printRTF( std::ostream& output ) const
{
    LQIO::SRVN::RTF srvn( *getDOM(), remapEntities(), Flags::print[CONFIDENCE_INTERVALS].value.b );
    srvn.print( output );
    return output;
}



#if defined(TXT_OUTPUT)
/*
 * Dump the layers.
 */

std::ostream&
Model::printTXT( std::ostream& output ) const
{
    if ( Flags::print_submodels ) {
	for ( std::vector<Layer>::const_iterator layer = (_layers.begin() + 1); layer != _layers.end(); ++layer ) {
	    if ( !*layer ) continue;
	    const_cast<Layer&>(*layer).generateSubmodel().printSubmodel( output );
	}
    } else {
	printLayers( output );
    }
    return output;
}
#endif


std::ostream&
Model::printXML( std::ostream& output ) const
{
    remapEntities();		/* Reorder to our order */
    _document->print( output, LQIO::DOM::Document::XML_OUTPUT );	/* Don't output LQX code if running. */
    return output;
}

/*
 * Print out one layer at at time.  Used by most graphical output routines.
 */

std::ostream&
Model::printLayers( std::ostream& output ) const
{
    if ( queueing_output() ) {
	const int submodel = Flags::print[QUEUEING_MODEL].value.i;
	_layers.at(submodel).drawQueueingNetwork( output );
    } else {
	for ( std::vector<Group *>::iterator group = Group::__groups.begin(); group != Group::__groups.end(); ++group ) {
	    if ( (*group)->isPseudoGroup() ) {
		output << *(*group);		/* Draw first */
	    }
	}
	for ( std::vector<Group *>::iterator group = Group::__groups.begin(); group != Group::__groups.end(); ++group ) {
	    if ( !(*group)->isPseudoGroup() ) {
		output << *(*group);
	    }
	}

	for ( std::vector<Layer>::const_iterator layer = _layers.begin(); layer != _layers.end(); ++layer ) {
	    if ( !*layer ) continue;
#if defined(TXT_OUTPUT)
	    if ( Flags::print[OUTPUT_FORMAT].value.i == FORMAT_TXT ) {
		output << "---------- Layer " << layer->number() << " ----------" << std::endl;
	    }
#endif
	    output << *layer;
	}
	if ( _key ) {
	    output << *_key;
	}
	if ( _label ) {
	    output << *_label;
	}
    }
    return output;
}



std::ostream&
Model::printEEPICprologue( std::ostream& output )
{
    output << "\\documentclass{article}" << std::endl;
    output << "\\usepackage{epic,eepic}" << std::endl;
    output << "\\usepackage[usenames]{color}" << std::endl;
    output << "\\begin{document}" << std::endl;
    return output;
}

std::ostream&
Model::printEEPICepilogue( std::ostream& output )
{
    output << "\\end{document}" << std::endl;
    return output;
}



/*
 * For 4-up printing run...
 * pstops '4:0@.5(0in,5.5in)+1@.5(4.5in,5.5in)+2@.5(0in,0in)+3@.5(4.5in,0in)' multi.ps > new.ps
 */

std::ostream&
Model::printPostScriptPrologue( std::ostream& output, const std::string& title,
				unsigned left, unsigned top, unsigned right, unsigned bottom ) const
{
    output << "%!PS-Adobe-2.0" << std::endl;
    output << "%%Title: " << title << std::endl;
    output << "%%Creator: " << LQIO::io_vars.lq_toolname << " Version " << VERSION << std::endl;
#if defined(HAVE_CTIME)
    time_t tloc;
    time( &tloc );
    output << "%%CreationDate: " << ctime( &tloc );
#endif
    output << "%%For: " << _login << std::endl;
    output << "%%Invoked as: " << command_line << ' ' << title << std::endl;
    output << "%%Orientation: Portrait" << std::endl;
    output << "%%Pages: " << maxModelNumber << std::endl;
    output << "%%BoundingBox: " << left << ' ' << top << ' ' << right << ' ' << bottom << std::endl;
    output << "%%BeginSetup" << std::endl;
    output << "%%IncludeFeature: *PageSize Letter" << std::endl;
    output << "%%EndSetup" << std::endl;
    output << "%%Magnification: 0.7500" << std::endl;
    output << "%%EndComments" << std::endl;
    return output;
}

#if defined(SXD_OUTPUT)
/* Open Office output...
 * The output is somewhat more complicated because we have to write out a pile of crap.
 */
#if defined(WINNT)
#define MKDIR(a1,a2) mkdir( a1 )
#else
#define MKDIR(a1,a2) mkdir( a1, a2 )
#endif

const Model&
Model::printSXD( const char * file_name ) const
{
    /* Use basename of input file name */
    LQIO::Filename dir_name( file_name, "" );

    if ( MKDIR( dir_name().c_str(), S_IRWXU ) < 0 ) {
	std::ostringstream msg;
	msg << "Cannot create directory \"" << dir_name() << "\" - " << strerror( errno );
	throw std::runtime_error( msg.str() );
    } else {
	std::string meta_name = dir_name();
	meta_name += "/META-INF";
	if ( MKDIR( meta_name.c_str(), S_IRWXU ) < 0 ) {
	    std::ostringstream msg;
	    msg << "Cannot create directory \"" << meta_name << "\" - " << strerror( errno );
	    rmdir( dir_name().c_str() );
	    throw std::runtime_error( msg.str() );
	} else try {
		printSXD( file_name, dir_name(), "META-INF/manifest.xml", &Model::printSXDManifest );
		printSXD( file_name, dir_name(), "content.xml", &Model::printSXD );
		printSXD( file_name, dir_name(), "meta.xml", &Model::printSXDMeta );
		printSXD( file_name, dir_name(), "styles.xml", &Model::printSXDStyles );
		printSXD( file_name, dir_name(), "settings.xml", &Model::printSXDSettings );
		printSXD( file_name, dir_name(), "mimetype", &Model::printSXDMimeType );
		rmdir( meta_name.c_str() );
	    }
	    catch ( const std::runtime_error &error ) {
		rmdir( meta_name.c_str() );
		rmdir( dir_name().c_str() );
		throw;
	    }

	rmdir( dir_name().c_str() );
    }
    return *this;
}

const Model&
Model::printSXD( const std::string& dst_name, const std::string& dir_name, const char * file_name, const printSXDFunc aFunc ) const
{
    std::string pathname = dir_name;
    pathname += "/";
    pathname += file_name;

    std::ofstream output;
    output.open( pathname.c_str(), std::ios::out );
    if ( !output ) {
	std::ostringstream msg;
	msg << "Cannot open output file \"" << pathname << "\" - " << strerror( errno );
	throw std::runtime_error( msg.str() );
    } else {
	/* Write out all other XML goop needed */
	(this->*aFunc)( output );
	output.close();

#if !defined(WINNT)
	std::ostringstream command;
	command << "cd " << dir_name << "; zip -r ../" << dst_name << " " << file_name;
	int rc = system( command.str().c_str() );
	unlink( pathname.c_str() );	/* Delete now. */
	if ( rc != 0 ) {
	    std::ostringstream msg;
	    msg << "Cannot execute \"" << command.str() << "\" - ";
	    if ( rc < 0 ) {
		msg << strerror( errno );
	    } else {
		msg << "status=0x" << std::hex << rc;
	    }
	    throw std::runtime_error( msg.str() );
	}
#endif
    }
    return *this;
}


std::ostream&
Model::printSXDMeta( std::ostream& output ) const
{
    char buf[32];
    unsigned int count = 0;
    for ( std::vector<Layer>::const_iterator layer = _layers.begin(); layer != _layers.end(); ++layer ) {
	count += layer->entities().size();
    }

    output << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>" << std::endl;
    output << "<!DOCTYPE office:document-meta PUBLIC \"-//OpenOffice.org//DTD OfficeDocument 1.0//EN\" \"office.dtd\">" << std::endl;
    output << "<office:document-meta xmlns:office=\"http://openoffice.org/2000/office\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:meta=\"http://openoffice.org/2000/meta\" xmlns:presentation=\"http://openoffice.org/2000/presentation\" xmlns:fo=\"http://www.w3.org/1999/XSL/Format\" office:version=\"1.0\">" << std::endl;
    output << "<office:meta>" << std::endl;
#if defined(HAVE_CTIME)
    time_t tloc;
    time( &tloc );
    strftime( buf, 32, "%Y-%m-%dT%T", localtime( &tloc ) );
#endif
    output << "<dc:title>" << _inputFileName << "</dc:title>" << std::endl;
    output << "<dc:comment>" << getDOM()->getModelComment() << "</dc:comment>" << std::endl;
    output << "<dc:creator>" << _login << "</dc:creator>" << std::endl;
    output << "<dc:date>" << buf << "</dc:date>" << std::endl;
    output << "<dc:language>en-US</dc:language>" << std::endl;

    output << "<meta:generator>" << LQIO::io_vars.lq_toolname << " Version " << VERSION << "</meta:generator>" << std::endl;
    output << "<meta:creation-date>" << buf << "</meta:creation-date>" << std::endl;
    output << "<meta:editing-cycles>1</meta:editing-cycles>" << std::endl;
#if defined(HAVE_SYS_TIMES_H)
    struct tms run_time;
    double stop_clock = times( &run_time );
    strftime( buf, 32, "PT%MM%SS", localtime( &tloc ) );
    output << "<meta:editing-duration>" << buf << "</meta:editing-duration>" << std::endl;
#endif
    output << "<meta:user-defined meta:name=\"Info 1\">" << command_line << "</meta:user-defined>" << std::endl;
    output << "<meta:user-defined meta:name=\"Info 2\"/>" << std::endl;
    output << "<meta:user-defined meta:name=\"Info 3\"/>" << std::endl;
    output << "<meta:user-defined meta:name=\"Info 4\"/>" << std::endl;
    output << "<meta:document-statistic meta:object-count=\"" << count << "\"/>" << std::endl;
    output << "</office:meta>" << std::endl;
    output << "</office:document-meta>" << std::endl;

    return output;
}

std::ostream&
Model::printSXDMimeType( std::ostream& output ) const
{
    output << "application/vnd.sun.xml.draw" << std::endl;
    return output;
}

std::ostream&
Model::printSXDSettings( std::ostream& output ) const
{
    set_indent(0);
    output << indent( 0 ) << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>" << std::endl;
    output << indent( 0 ) << "<!DOCTYPE office:document-settings PUBLIC \"-//OpenOffice.org//DTD OfficeDocument 1.0//EN\" \"office.dtd\">" << std::endl;
    output << indent( 1 ) << "<office:document-settings xmlns:office=\"http://openoffice.org/2000/office\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" xmlns:presentation=\"http://openoffice.org/2000/presentation\" xmlns:config=\"http://openoffice.org/2001/config\" xmlns:fo=\"http://www.w3.org/1999/XSL/Format\" office:version=\"1.0\">" << std::endl;
    output << indent( 1 ) << "<office:settings>" << std::endl;
    output << indent( -1 ) << "</office:settings>" << std::endl;
    output << indent( -1 ) << "</office:document-settings>" << std::endl;
    return output;
}

std::ostream&
Model::printSXDStyles( std::ostream& output ) const
{
    set_indent(0);
    return SXD::printStyles( output );
}

std::ostream&
Model::printSXDManifest( std::ostream& output ) const
{
    output << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>" << std::endl;
    output << "<!DOCTYPE manifest:manifest PUBLIC \"-//OpenOffice.org//DTD Manifest 1.0//EN\" \"Manifest.dtd\">" << std::endl;
    output << "<manifest:manifest xmlns:manifest=\"http://openoffice.org/2001/manifest\">" << std::endl;
    output << "<manifest:file-entry manifest:media-type=\"application/vnd.sun.xml.draw\" manifest:full-path=\"/\"/>" << std::endl;
    output << "<manifest:file-entry manifest:media-type=\"\" manifest:full-path=\"Pictures/\"/>" << std::endl;
    output << "<manifest:file-entry manifest:media-type=\"text/xml\" manifest:full-path=\"content.xml\"/>" << std::endl;
    output << "<manifest:file-entry manifest:media-type=\"text/xml\" manifest:full-path=\"styles.xml\"/>" << std::endl;
    output << "<manifest:file-entry manifest:media-type=\"text/xml\" manifest:full-path=\"meta.xml\"/>" << std::endl;
    output << "<manifest:file-entry manifest:media-type=\"text/xml\" manifest:full-path=\"settings.xml\"/>" << std::endl;
    output << "</manifest:manifest>" << std::endl;
    return output;
}
#endif

/* ------------------------------------------------------------------------ */

Model::Count&
Model::Count::operator=( unsigned int value )
{
    _tasks = value;
    _processors = value;
    _entries = value;
    _activities = value;
    return *this;
}

Model::Count&
Model::Count::operator+=( const Model::Count& addend )
{
    _tasks += addend._tasks;
    _processors += addend._processors;
    _entries += addend._entries;
    _activities += addend._activities;
    return *this;
}

Model::Count&
Model::Count::operator()( const Entity * entity ) 
{
    const Task * aTask = dynamic_cast<const Task *>(entity);
    if ( aTask ) {
	_tasks      += 1;
	_entries    += aTask->nEntries();
	_activities += aTask->nActivities();
    } else if ( dynamic_cast<const Processor *>(entity) ) {
	_processors += 1;
    }
    return *this;
}

/*
 *
 */

std::ostream&
Model::printStatistics( std::ostream& output, const char * filename ) const
{
    if ( filename ) {
	output << filename << ":" << std::endl;
    }

    output << "  Layers: " << _layers.size() << std::endl
	   << "  Tasks: " << _total.tasks() << std::endl
	   << "  Processors: " << _total.processors() << std::endl
	   << "  Entries: " << _total.entries() << std::endl
	   << "  Phases:" << phaseCount[1] << "," << phaseCount[2] << "," << phaseCount[3] << std::endl;
    if ( _total.activities() > 0 ) {
	output << "  Activites: " << _total.activities() << std::endl;
    }
    output << "  Customers: ";

    unsigned i = 0;
    for ( std::set<Task *>::const_iterator task = Task::__tasks.begin(); task != Task::__tasks.end(); ++task ) {
	if ( (*task)->isReferenceTask() ) {
	    i += 1;
	    if ( i > 1 ) {
		output << ",";
	    }
	    output << (*task)->copies();
	}
    }
    output << std::endl;
    if ( openArrivalCount > 0 ) {
	output << "  Open Arrivals: " << openArrivalCount << std::endl;
    }
    if ( rendezvousCount[0] ) {
	output << "  Rendezvous: " << rendezvousCount[1] << "," << rendezvousCount[2] << "," << rendezvousCount[3] << std::endl;
    }
    if ( sendNoReplyCount[0] ) {
	output << "  Send-no-reply: " << sendNoReplyCount[1] << "," << sendNoReplyCount[2] << "," << sendNoReplyCount[3] << std::endl;
    }
    if ( forwardingCount ) {
	output << "  Forwarding: " << forwardingCount << std::endl;
    }
    return output;
}


std::ostream&
Model::printOverallStatistics( std::ostream& output )
{
    for ( int i = 0; i < N_STATS; ++i ) {
	if ( stats[i].sum() > 0 ) {
	    output << stats[i] << std::endl;
	}
    }
    return output;
}


std::ostream&
Model::printSummary( std::ostream& output ) const
{
    if ( _inputFileName.size() ) {
	output << _inputFileName << ":";
    }
    if ( graphical_output() ) {
	output << "  width=\"" << to_inches( right() ) << "\", height=\"" << to_inches( top() ) << "\"";
    }
    output << std::endl;

    if ( Group::__groups.size() == 0 && Flags::print_submodels ) {
	for ( std::vector<Layer>::const_iterator layer = (_layers.begin()+1); layer != _layers.end(); ++layer ) {
	    if ( !*layer  || layer->number() == 1 ) continue;
	    output << "    " << std::setw( 2 ) << layer->number() - 1 << ": ";
	    const_cast<Layer&>(*layer).generateSubmodel().printSubmodelSummary( output );
	}
    } else {
	for ( std::vector<Layer>::const_iterator layer = _layers.begin(); layer != _layers.end(); ++layer ) {
	    if ( !*layer ) continue;
	    std::cerr << "    " << std::setw( 2 ) << layer->number() << ": ";
	    layer->printSummary( output ) << std::endl;
	}
    }

    return output;
}

/*----------------------------------------------------------------------*/
/*                             Batch Model.                             */
/*----------------------------------------------------------------------*/


/*
 * Stick tasks and  processors into layers based on their call depth.
 * This corresponds to the batched model used in LQNS.
 */

Model&
Batch_Model::layerize()
{
    for ( std::set<Task *>::const_iterator task = Task::__tasks.begin(); task != Task::__tasks.end(); ++task ) {
	if ( !(*task)->pathTest() ) continue;

	_layers.at((*task)->level()).append((*task));

	/* find who calls me and stick them in too */
	for ( std::vector<Entry *>::const_iterator entry = (*task)->entries().begin(); entry != (*task)->entries().end(); ++entry ) {
	    if ( (graphical_output() || queueing_output()) && (*entry)->hasOpenArrivalRate() ) {
		_layers.at((*task)->level()-1).append( new OpenArrivalSource(*entry) );
	    }
	}

	Processor * aProcessor = const_cast<Processor *>((*task)->processor());
	if ( aProcessor && aProcessor->isInteresting() ) {
	    _layers.at(aProcessor->level()).append( aProcessor );
	}
    }

    return *this;
}

/*----------------------------------------------------------------------*/
/*                              HWSW Model.                             */
/*----------------------------------------------------------------------*/


/*
 * Stick tasks into the top two layers (the software model);
 * Stick all processors into the bottom layer (the hardware model).
 */

Model&
HWSW_Model::layerize()
{
    for ( std::set<Task *>::const_iterator task = Task::__tasks.begin(); task != Task::__tasks.end(); ++task ) {
	if ( !(*task)->pathTest() ) {
	    continue;
	} else if ( (*task)->level() > SERVER_LEVEL ) {
	    (*task)->setLevel( SERVER_LEVEL );
	}

	_layers.at((*task)->level()).append((*task));

	/* find who calls me and stick them in too */
	for ( std::vector<Entry *>::const_iterator entry = (*task)->entries().begin(); entry != (*task)->entries().end(); ++entry ) {
	    if ( (graphical_output() || queueing_output()) && (*entry)->hasOpenArrivalRate() ) {
		_layers.at(CLIENT_LEVEL).append( new OpenArrivalSource(*entry) );
	    }
	}

	Processor * aProcessor = const_cast<Processor *>((*task)->processor());
	if ( aProcessor && aProcessor->isInteresting() ) {
	    aProcessor->setLevel( PROCESSOR_LEVEL );
	    _layers.at(PROCESSOR_LEVEL).append( aProcessor );
	}
    }

    return *this;
}

/*----------------------------------------------------------------------*/
/*                          Strict (MOL) Model.                         */
/*----------------------------------------------------------------------*/


/*
 * Stick tasks into all but the bottom layer (the software model);
 * stick all processors into the bottom layer (the hardware model).
 * This technique corresonds to Rolia's.
 */

Model&
MOL_Model::layerize()
{
    const unsigned PROC_LEVEL = nLayers();

    for ( std::set<Task *>::const_iterator task = Task::__tasks.begin(); task != Task::__tasks.end(); ++task ) {
	if ( !(*task)->pathTest() ) continue;

	_layers.at((*task)->level()).append((*task));

	/* find who calls me and stick them in too */
	for ( std::vector<Entry *>::const_iterator entry = (*task)->entries().begin(); entry != (*task)->entries().end(); ++entry ) {
	    if ( (graphical_output() || queueing_output()) && (*entry)->hasOpenArrivalRate() ) {
		_layers.at((*task)->level()-1).append( new OpenArrivalSource(*entry) );
	    }
	}

	Processor * aProcessor = const_cast<Processor *>((*task)->processor());
	if ( aProcessor && aProcessor->isInteresting() ) {
	    aProcessor->setLevel( PROC_LEVEL );
	    _layers.at(PROC_LEVEL).append( aProcessor );
	}
    }

    return *this;
}

/*----------------------------------------------------------------------*/
/*                            Group Model.                              */
/*----------------------------------------------------------------------*/


/*
 * Each group consists of a set of processors and their tasks.  These
 * are all formatted then justified as needed.
 */

Model&
Group_Model::justify()
{
    _origin.x( 0.0 );

    /* Now sort by groups */
    _extent.x( for_each( Group::__groups.begin(), Group::__groups.end(), Justify( _layers.size() ) ).extent() );

    for ( std::vector<Layer>::iterator layer = _layers.begin(); layer != _layers.end(); ++layer ) {
	layer->moveLabelTo( right(), layer->height() / 2.0 ).sort( (compare_func_ptr)(&Entity::compareCoord) );
    }
    return *this;
}


void
Group_Model::Justify::operator()( Group * group )
{
    group->format().label().resizeBox().positionLabel();

    /* The next column starts here.  PseudoGroups (for group
     * scheduling) are ignored if they have no default tasks */
    
    if ( _x > 0 ) {
	_x += Flags::print[X_SPACING].value.f;
    }
    if ( !group->isPseudoGroup() ) {
	group->moveGroupBy( _x, 0.0 );
	group->depth( (_max_level+1) * 10 );
	_x += group->width();
    } else {
	_x = group->x() + group->width();
	group->depth( (_max_level+2) * 10 );
    }
}

/*----------------------------------------------------------------------*/
/*                    Processors followed by Tasks                      */
/*----------------------------------------------------------------------*/


/*
 * This version of justify moves all of the processors and all of the tasks together.
 */

Model&
ProcessorTask_Model::justify()
{
    const unsigned MAX_LEVEL = _layers.size();
    double procWidthPts = 0.0;
    double taskWidthPts = 0.0;

    std::vector<Layer> procLayer(MAX_LEVEL);
    std::vector<Layer> taskLayer(MAX_LEVEL);

    for ( std::vector<Layer>::reverse_iterator layer = _layers.rbegin(); layer != _layers.rend(); ++layer ) {
	if ( !*layer ) continue;
	const unsigned int i = layer->number();

	for ( std::vector<Entity *>::const_iterator entity = layer->entities().begin(); entity != layer->entities().end(); ++entity ) {
	    if ( dynamic_cast<Processor *>((*entity)) ) {
		procLayer[i].append( *entity );
	    } else {
		taskLayer[i].append( *entity );
	    }
	}

	/* Move all processors in a given layer together */
	if ( procLayer[i].size() ) {
	    procLayer[i].reformat();
	    procWidthPts = std::max( procWidthPts, procLayer[i].x() + procLayer[i].width() );
	}
	/* Move all tasks in a given layer together */
	if ( taskLayer[i].size() ) {
	    taskLayer[i].reformat();
	    taskWidthPts = std::max( taskWidthPts, taskLayer[i].x() + taskLayer[i].width() );
	}
    }

    /* Set max width */

    _extent.x( procWidthPts + Flags::print[X_SPACING].value.f + taskWidthPts );

    /* Now, move all tasks */

    for ( std::vector<Layer>::iterator layer = _layers.begin(); layer != _layers.end(); ++layer ) {
	if ( !*layer ) continue;
	const unsigned int i = layer->number();

	switch ( Flags::node_justification ) {
	case ALIGN_JUSTIFY:
	case CENTER_JUSTIFY:
	    justify2( procLayer[i], taskLayer[i], (right() - (taskLayer[i].width() + procLayer[i].width() + Flags::print[X_SPACING].value.f)) / 2.0 );
	    break;
	case RIGHT_JUSTIFY:
	    justify2( procLayer[i], taskLayer[i],  right() - (taskLayer[i].width() + procLayer[i].width() + Flags::print[X_SPACING].value.f) );
	    break;
	case LEFT_JUSTIFY:
	    justify2( procLayer[i], taskLayer[i],  0.0 );
	    break;
	case DEFAULT_JUSTIFY:
	    if ( Flags::print[LAYERING].value.i == LAYERING_PROCESSOR_TASK ) {
		procLayer[i].justify( procWidthPts, RIGHT_JUSTIFY );
		taskLayer[i].justify( taskWidthPts, LEFT_JUSTIFY ).moveBy( procWidthPts + Flags::print[X_SPACING].value.f, 0.0 );
	    } else {
		taskLayer[i].justify( taskWidthPts, RIGHT_JUSTIFY );
		procLayer[i].justify( procWidthPts, LEFT_JUSTIFY ).moveBy( taskWidthPts + Flags::print[X_SPACING].value.f, 0.0 );
	    }
	    break;
	}
    }

    return *this;
}


const Model&
ProcessorTask_Model::justify2( Layer &procLayer, Layer &taskLayer, const double offset ) const
{
    if ( Flags::print[LAYERING].value.i == LAYERING_PROCESSOR_TASK ) {
	procLayer.moveBy( offset, 0.0 );
	taskLayer.moveBy( offset + procLayer.width() + Flags::print[X_SPACING].value.f, 0.0 );
    } else {
	taskLayer.moveBy( offset, 0.0 );
	procLayer.moveBy( offset + taskLayer.width() + Flags::print[X_SPACING].value.f, 0.0 );
    }
    return *this;
}

/*----------------------------------------------------------------------*/
/*                             SRVN Model.                              */
/*----------------------------------------------------------------------*/


/*
 * The trick for the SRVN Model is to retain the layering from the
 * topological sorter, but to treat each server as its own submodel.
 * All of the servers are ordered based on their level then entityID.
 * We only select the server who matches the submodel.
 */

bool
SRVN_Model::selectSubmodel( const unsigned submodel )
{
    /* Build the list of all servers for this model */

    std::multiset<Entity *,lt_submodel> servers;
    for ( std::set<Task *>::const_iterator task = Task::__tasks.begin(); task != Task::__tasks.end(); ++task ) {
	if ( (*task)->isReferenceTask() || (*task)->level() <= 0 ) continue;
	servers.insert( *task );
    }
    for ( std::set<Processor *>::const_iterator processor = Processor::__processors.begin(); processor != Processor::__processors.end(); ++processor ) {
	if ( (*processor)->level() <= 0 ) continue;
	servers.insert( *processor );
    }

    unsigned int s = 1;
    for ( std::multiset<Entity *,lt_submodel>::const_iterator server = servers.begin(); server != servers.end(); ++server, ++s ) {
	if ( s == submodel ) {
	    (*server)->isSelected( true );
	    return true;
	}
    }

    return false;
}

/*----------------------------------------------------------------------*/
/*                          Squashed Model.                             */
/*----------------------------------------------------------------------*/


/*
 * The trick for squashed layering is to simply jam all tasks in
 * layer 1 and all processors in layer 2.
 */

bool
Squashed_Model::generate()
{
    /*
     * Now go through and reset the level field on all the objects.
     */

    for ( std::set<Task *>::const_iterator task = Task::__tasks.begin(); task != Task::__tasks.end(); ++task ) {
	if ( (*task)->hasActivities() ) {
	    (*task)->generate();
	}
	if ( (*task)->level() > CLIENT_LEVEL ) {
	    (*task)->setLevel( CLIENT_LEVEL );
	}
    }
    for ( std::set<Processor *>::const_iterator nextProcessor = Processor::__processors.begin(); nextProcessor != Processor::__processors.end(); ++nextProcessor ) {
	Processor * aProcessor = *nextProcessor;
	if ( aProcessor->level() == 0 ) {
	    LQIO::solution_error( LQIO::WRN_NOT_USED, "Processor", aProcessor->name().c_str() );
	} else {
	    aProcessor->setLevel( SERVER_LEVEL );
	}
    }

    _layers.resize( SERVER_LEVEL );

    return Flags::print[IGNORE_ERRORS].value.b || !LQIO::io_vars.anError();
}

Model&
Squashed_Model::justify()
{
    Model::justify();
    for_each( Group::__groups.begin(), Group::__groups.end(), Justify() );
    return *this;
}

void
Squashed_Model::Justify::operator()( Group * group )
{
    group->format().label().resizeBox().positionLabel();
}

/* ------------------------------------------------------------------------ */

Model::Stats::Stats()
    : n(0), x(0), x_sqr(0), log_x(0), one_x(0), min(MAXDOUBLE), max(-MAXDOUBLE), myFunc(0)
{
    min_filename = "";
    max_filename = "";
}



Model::Stats&
Model::Stats::accumulate( double value, const std::string& filename )
{
    n += 1;
    x += value;
    x_sqr += value * value;
    log_x += log( value );
    one_x += 1.0 / value;
    if ( value < min ) {
	min = value;
	min_filename = filename;
    } else if ( value == min && min_filename.find( filename ) == std::string::npos ) {
	min_filename += ", ";
	min_filename += filename;
    }
    if ( value > max ) {
	max = value;
	max_filename = filename;
    } else if ( value == max && max_filename.find( filename ) == std::string::npos ) {
	max_filename += ", ";
	max_filename += filename;
    }
    return *this;
}


Model::Stats&
Model::Stats::accumulate( const Model * aModel, const std::string& filename )
{
    assert( aModel && myFunc );
    return accumulate( static_cast<double>((aModel->*myFunc)()), filename );
}


/*
 * Compute population standard deviation.
 */

std::ostream&
Model::Stats::print( std::ostream& output) const
{
    output << myName << ":" << std::endl;
    double stddev = 0.0;
    if ( n > 1 ) {
	const double numerator = x_sqr - ( x * x ) / static_cast<double>(n);
	if ( numerator > 0.0 ) {
	    stddev = sqrt( numerator / static_cast<double>( n ) );
	}
    }
    output << "  mean   = " << x / static_cast<double>(n) << std::endl;
    output << "  geom   = " << exp( log_x / static_cast<double>(n) ) << std::endl;	/* Geometric mean */
    output << "  stddev = " << stddev << std::endl;
    output << "  max    = " << max << " (" << max_filename << ")" << std::endl;
    output << "  min    = " << min << " (" << min_filename << ")" << std::endl;
    return output;
}

static std::ostream&
print_comment_str( std::ostream& output, const char * prefix, const LQIO::DOM::ExternalVariable& var )
{
    output << prefix;
    const char * s = 0;
    if ( var.getString( s ) && s ) {
	for ( ; *s; ++s ) {
	    output << *s;
	    if ( *s == '\n' ) {
		output << prefix;
	    }
	}
    }
    return output;
}

static std::ostream&
to_inches_str( std::ostream& output, const double value )
{
    switch ( Flags::print[OUTPUT_FORMAT].value.i ) {
    case FORMAT_FIG:
    case FORMAT_PSTEX:
	output << value / (FIG_SCALING * PTS_PER_INCH);
	break;

    case FORMAT_EEPIC:
	output << value / (EEPIC_SCALING * PTS_PER_INCH);
	break;
#if defined(EMF_OUTPUT)
    case FORMAT_EMF:
	output << value / (EMF_SCALING * PTS_PER_INCH);
	break;
#endif
#if defined(SVG_OUTPUT)
    case FORMAT_SVG:
	output << value / (SVG_SCALING * PTS_PER_INCH);
	break;
#endif
#if defined(SXD_OUTPUT)
    case FORMAT_SXD:
	output << value / (SXD_SCALING * PTS_PER_INCH);
	break;
#endif
    default:
	output << value / PTS_PER_INCH;
	break;
    }
    return output;
}



static CommentManip print_comment( const char * prefix, const LQIO::DOM::ExternalVariable& var )
{
    return CommentManip( &print_comment_str, prefix, var );
}


static DoubleManip
to_inches( const double value )
{
    return DoubleManip( &to_inches_str, value );
}
