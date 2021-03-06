/* -*- c++ -*-
 * $HeadURL: http://rads-svn.sce.carleton.ca:8080/svn/lqn/trunk-V5/lqns/processor.cc $
 *
 * Everything you wanted to know about a task, but were afraid to ask.
 *
 * Copyright the Real-Time and Distributed Systems Group,
 * Department of Systems and Computer Engineering,
 * Carleton University, Ottawa, Ontario, Canada. K1S 5B6
 *
 * November, 1994
 *
 * ------------------------------------------------------------------------
 * $Id: processor.cc 14140 2020-11-25 20:24:15Z greg $
 * ------------------------------------------------------------------------
 */

#include "dim.h"
#include <string>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <lqio/input.h>
#include <lqio/labels.h>
#include <lqio/error.h>
#include "activity.h"
#include "call.h"
#include "entity.h"
#include "entry.h"
#include "errmsg.h"
#include "fpgoop.h"
#include "lqns.h"
#include "model.h"
#include "multserv.h"
#include "mva.h"
#include "open.h"
#include "pragma.h"
#include "processor.h"
#include "server.h"
#include "submodel.h"
#include "task.h"

bool Processor::__prune = false;


/* ------------------------ Constructors etc. ------------------------- */

Processor::Processor( LQIO::DOM::Processor* dom )
    : Entity( dom, std::vector<Entry *>() )
{
}


/*
 * Destructor...
 */

Processor::~Processor()
{
}

/* ------------------------ Instance Methods -------------------------- */

bool
Processor::check() const
{

    /* Check replication */

    const unsigned int proc_replicas = this->replicas();
    for ( std::set<const Task *>::const_iterator task = tasks().begin(); task != tasks().end(); ++task ) {
	double temp = static_cast<double>((*task)->replicas()) / static_cast<double>(proc_replicas);
	if ( trunc( temp ) != temp  ) {			/* Integer multiple */
	    LQIO::solution_error( ERR_REPLICATION_PROCESSOR, (*task)->replicas(), (*task)->name().c_str(), proc_replicas, name().c_str() );
	}
    }
    
    if ( !schedulingIsOk( validScheduling() ) ) {
	LQIO::solution_error( LQIO::WRN_SCHEDULING_NOT_SUPPORTED,
			      scheduling_label[static_cast<unsigned int>(scheduling())].str,
			      getDOM()->getTypeName(),
			      name().c_str() );
	getDOM()->setSchedulingType(defaultScheduling());
    }

    if ( scheduling() == SCHEDULE_DELAY ) {
	if ( copies() != 1 ) {
	    solution_error( LQIO::WRN_INFINITE_MULTI_SERVER, "Processor", name().c_str(), copies() );
	    getDOM()->setCopies(new LQIO::DOM::ConstantExternalVariable(1.0));
	}
    } else if ( !Pragma::defaultProcessorScheduling() && copies() == 1 ) {
	/* Change scheduling type for uni-processors (usually from FCFS to PS) */
	getDOM()->setSchedulingType(Pragma::processorScheduling());
    }
    return true;
}



/*
 * Denote whether this station belongs to the open, closed, or mixed
 * models when performing the MVA solution.
 */

Processor&
Processor::configure( const unsigned nSubmodels )
{
    if ( nEntries() == 0 ) {
	if ( isInteresting() || nClients() == 0 ) {
	    LQIO::solution_error( LQIO::WRN_NO_TASKS_DEFINED_FOR_PROCESSOR, name().c_str() );
	}
	return *this;
    }

    std::vector<Entry *>::const_iterator entry = entries().begin();
    double minS = (*entry)->serviceTime();
    double maxS = (*entry)->serviceTime();
    for ( ++entry; entry != entries().end(); ++entry ) {
	minS = std::min( minS, (*entry)->serviceTime() );
	maxS = std::max( maxS, (*entry)->serviceTime() );
    }
    if ( maxS > 0. && minS / maxS < 0.1
	 && !schedulingIsOk( SCHED_PS_BIT|SCHED_PS_HOL_BIT|SCHED_PS_PPR_BIT|SCHED_DELAY_BIT ) ) {
	LQIO::solution_error( ADV_SERVICE_TIME_RANGE, getDOM()->getTypeName(), name().c_str(), minS, maxS );
    }
    Entity::configure( nSubmodels );
    if ( Pragma::forceMultiserver( Pragma::FORCE_PROCESSORS ) ) {
	attributes.variance = 0;
    }
    
    return *this;
}



/*
 * Initialize population levels.  Since processors are servers, it
 * doesn't really matter.  However, we do have to figure out whether
 * we're in an open or a closed model (or both).
 */

Processor&
Processor::initPopulation()
{
    _population = static_cast<double>(copies());	/* Doesn't matter... */

    for ( std::set<const Task *>::const_iterator task = tasks().begin(); task != tasks().end(); ++task ) {
	std::set<Task *> sources;		/* Cltn of tasks already visited. */
	if ( (*task)->countCallers( sources ) > 0. ) {
	    isInClosedModel( true );
	}
	if ( (*task)->isInfinite() && (*task)->isInOpenModel() ) {
	    isInOpenModel( true );
	}
    }
    return *this;
}


double
Processor::rate() const
{
    double value = 1.0;
    if ( getDOM()->hasRate() ) {
	try {
	    value = getDOM()->getRateValue();
	}
	catch ( const std::domain_error& e ) {
	    solution_error( LQIO::ERR_INVALID_PARAMETER, "rate", getDOM()->getTypeName(), name().c_str(), e.what() );
	    throw_bad_parameter();
	}
    } 
    return value;
}



/*
 * Return the fan-in to this server from...
 */

unsigned
Processor::fanIn( const Task * aClient ) const
{
    return aClient->replicas() / replicas();
}

/*
 * Processors don't have a fanout ever 
 */

unsigned
Processor::fanOut( const Entity * aServer ) const
{
    throw should_not_implement( "Entity::fanOut", __FILE__, __LINE__ );
    return 1;
}

/*
 * Return true if we want to keep this processor.  Interesting processors
 * will likely have queues.
 */
 
bool
Processor::isInteresting() const
{
    if ( !__prune ) {
	return true;
    } else if ( isInfinite() ) {
	return false;
    } else {
	unsigned int sum = 0;
	for ( std::set<const Task *>::const_iterator next = tasks().begin(); next != tasks().end(); ++next ) {
	    const Task *aTask = *next;
	    if ( aTask->isInfinite() ) return true;
	    sum += aTask->copies();
	}
	return sum > copies();
    }
}


/*
 * Indicate whether the variance calculation should take place.  NOTE
 * that processors should not have the variance calculation set true,
 * so hasVariance is set in class Task rather than here.
 */

bool
Processor::hasVariance() const
{
    if ( Pragma::variance(Pragma::NO_VARIANCE)
	 || !Pragma::defaultProcessorScheduling()
	 || scheduling() == SCHEDULE_PS
	 || isMultiServer()
	 || isInfinite() ) {
	return false;
    } else {
	return std::any_of( entries().begin(), entries().end(), Predicate<Entry>( &Entry::hasVariance ) );
    }
}



/*
 * Return true if this processor can schedule tasks with priority.
 */

bool
Processor::hasPriorities() const
{
    return scheduling() == SCHEDULE_HOL
	|| scheduling() == SCHEDULE_PPR
	|| scheduling() == SCHEDULE_PS_HOL
	|| scheduling() == SCHEDULE_PS_PPR;
}


/*
 * Return the scheduling type allowed for this object.  Overridden by
 * subclasses if the scheduling type can be something other than FIFO.
 */

unsigned
Processor::validScheduling() const
{
    if ( isInfinite() ) {
	return (unsigned)-1;
    } else if ( isMultiServer() ) {
	return SCHED_PS_BIT|SCHED_FIFO_BIT;
    } else {
	return SCHED_FIFO_BIT|SCHED_PPR_BIT|SCHED_HOL_BIT|SCHED_PS_BIT|SCHED_PS_PPR_BIT|SCHED_PS_HOL_BIT;
    }
}


/*
 * Create (or recreate) a server.  If we're called a a second+ time,
 * and the station type changes, then we change the underlying
 * station.  We only return a station when we create one.
 */

Server *
Processor::makeServer( const unsigned nChains )
{
    if ( isInfinite() ) {

	/* ---------------- Infinite Servers ---------------- */

	if ( dynamic_cast<Infinite_Server *>(_station) ) return 0;
	_station = new Infinite_Server( nEntries(), nChains, maxPhase() );

    } else if ( isMultiServer() || Pragma::forceMultiserver( Pragma::FORCE_PROCESSORS ) ) {

	/* ---------------- Multi Servers ---------------- */

	if ( scheduling() == SCHEDULE_PS ) {

	    switch ( Pragma::multiserver() ) {
	    default:
	    case Pragma::DEFAULT_MULTISERVER:
	    case Pragma::CONWAY_MULTISERVER:
	    case Pragma::REISER_MULTISERVER:
	    case Pragma::REISER_PS_MULTISERVER:
	    case Pragma::SCHMIDT_MULTISERVER:
		if ( dynamic_cast<Reiser_PS_Multi_Server *>(_station) && _station->marginalProbabilitiesSize() == copies()) return 0;
		_station = new Reiser_PS_Multi_Server( copies(), nEntries(), nChains );
		break;

	    case Pragma::ROLIA_PS_MULTISERVER:
	    case Pragma::ROLIA_MULTISERVER:
		if ( dynamic_cast<Rolia_PS_Multi_Server *>(_station) ) return 0;
		_station = new Rolia_PS_Multi_Server( copies(), nEntries(), nChains );
		break;
	    }

	} else {

	    switch ( Pragma::multiserver() ) {
	    default:
	    case Pragma::DEFAULT_MULTISERVER:
		if ( copies() < 20 && nChains <= 5 ) {
		    if ( dynamic_cast<Conway_Multi_Server *>(_station) && _station->marginalProbabilitiesSize() == copies()) return 0;
		    _station = new Conway_Multi_Server( copies(), nEntries(), nChains );
		} else {
		    if ( dynamic_cast<Rolia_Multi_Server *>(_station) ) return 0;
		    _station = new Rolia_Multi_Server(  copies(), nEntries(), nChains );
		}
		break;

	    case Pragma::CONWAY_MULTISERVER:
		if ( dynamic_cast<Conway_Multi_Server *>(_station) && _station->marginalProbabilitiesSize() == copies()) return 0;
		_station = new Conway_Multi_Server( copies(), nEntries(), nChains );
		break;

	    case Pragma::REISER_MULTISERVER:
		if ( dynamic_cast<Reiser_Multi_Server *>(_station) && _station->marginalProbabilitiesSize() == copies()) return 0;
		_station = new Reiser_Multi_Server( copies(), nEntries(), nChains );
		break;

	    case Pragma::REISER_PS_MULTISERVER:
		if ( dynamic_cast<Reiser_PS_Multi_Server *>(_station) && _station->marginalProbabilitiesSize() == copies()) return 0;
		_station = new Reiser_PS_Multi_Server( copies(), nEntries(), nChains );
		break;

	    case Pragma::ROLIA_MULTISERVER:
		if ( dynamic_cast<Rolia_Multi_Server *>(_station) ) return 0;
		_station = new Rolia_Multi_Server( copies(), nEntries(), nChains );
		break;

	    case Pragma::ROLIA_PS_MULTISERVER:
		if ( dynamic_cast<Rolia_PS_Multi_Server *>(_station) ) return 0;
		_station = new Rolia_PS_Multi_Server( copies(), nEntries(), nChains );
		break;

	    case Pragma::BRUELL_MULTISERVER:
		if ( dynamic_cast<Bruell_Multi_Server *>(_station) && _station->marginalProbabilitiesSize() == copies()) return 0;
		_station = new Bruell_Multi_Server( copies(), nEntries(), nChains );
		break;

	    case Pragma::SCHMIDT_MULTISERVER:
		if ( dynamic_cast<Schmidt_Multi_Server *>(_station) && _station->marginalProbabilitiesSize() == copies()) return 0;
		_station = new Schmidt_Multi_Server( copies(), nEntries(), nChains );
		break;
	    }
	}
    } else {
	switch ( scheduling() ) {
	default:
	case SCHEDULE_FIFO:
	    if ( hasVariance() ) {
		if ( dynamic_cast<HVFCFS_Server *>(_station) ) return 0;
		_station = new HVFCFS_Server( nEntries(), nChains, maxPhase() );
	    } else {
		if ( dynamic_cast<FCFS_Server *>(_station) ) return 0;
		_station = new FCFS_Server( nEntries(), nChains, maxPhase() );
	    }
	    break;

	case SCHEDULE_PPR:
	    if ( hasVariance() ) {
		if ( dynamic_cast<PR_HVFCFS_Server *>(_station) ) return 0;
		_station = new PR_HVFCFS_Server( nEntries(), nChains, maxPhase() );
	    } else {
		if ( dynamic_cast<PR_FCFS_Server *>(_station) ) return 0;
		_station = new PR_FCFS_Server( nEntries(), nChains, maxPhase() );
	    }
	    break;

	case SCHEDULE_HOL:
	    if ( hasVariance() ) {
		if ( dynamic_cast<HOL_HVFCFS_Server *>(_station) ) return 0;
		_station = new HOL_HVFCFS_Server( nEntries(), nChains, maxPhase() );
	    } else {
		if ( dynamic_cast<HOL_FCFS_Server *>(_station) ) return 0;
		_station = new HOL_FCFS_Server( nEntries(), nChains, maxPhase() );
	    }
	    break;

	case SCHEDULE_PS:
	    if ( dynamic_cast<PS_Server *>(_station) ) return 0;
	    _station = new PS_Server( nEntries(), nChains, maxPhase() );
	    break;

	case SCHEDULE_PS_HOL:
	    if ( dynamic_cast<HOL_PS_Server *>(_station) ) return 0;
	    _station = new HOL_PS_Server( nEntries(), nChains, maxPhase() );
	    break;

	case SCHEDULE_PS_PPR:
	    if ( dynamic_cast<PR_PS_Server *>(_station) ) return 0;
	    _station = new PR_PS_Server( nEntries(), nChains, maxPhase() );
	    break;
	}
    }

    return _station;
}


/*
 * Check results for sanity.
 */

const Entity&
Processor::sanityCheck() const
{
    Entity::sanityCheck();
    if ( utilization() != _utilization ) {
#if 0
	std::cerr << "Utilization mismatch: derived=" << utilization()
		  << ", MVA=" << _utilization << std::endl;
#endif
    }
    return *this;
}


Entity&
Processor::saveServerResults( const MVASubmodel& submodel, double relax )
{
    Entity::saveServerResults( submodel, relax );

    const Server * station = serverStation();
    const std::set<Task *>& clients = submodel.getClients();
    const unsigned int n = submodel.number();

    _utilization = 0.0;
    for ( std::set<Task *>::const_iterator client = clients.begin(); client != clients.end(); ++client ) {
	const ChainVector& chain = (*client)->clientChains( n );

	if ( submodel.closedModel != nullptr ) {
	    for ( unsigned ix = 1; ix <= chain.size(); ++ix ) {
		const unsigned k = chain[ix];
		if ( hasServerChain(k) ) {
		    _utilization += submodel.closedModel->utilization( *station, k );
		}
	    }
	}
	if ( submodel.openModel ) {
	    _utilization += submodel.openModel->utilization( *station );
	}
    }
    
    return *this;
}


const Processor&
Processor::insertDOMResults(void) const
{
    double sumOfProcUtil = 0.0;
    for ( std::set<const Task *>::const_iterator task = tasks().begin(); task != tasks().end(); ++task ) {

	const std::vector<Entry *>& entries = (*task)->entries();
	for ( std::vector<Entry *>::const_iterator entry = entries.begin(); entry != entries.end(); ++entry ) {
	    if ((*entry)->isStandardEntry()) {
		sumOfProcUtil += (*entry)->processorUtilization();
	    }
	}
	const std::vector<Activity *>& activities = (*task)->activities();
	sumOfProcUtil += std::accumulate( activities.begin(), activities.end(), 0., add_using<Activity>( &Activity::processorUtilization ) );
    }

    if ( getDOM() ) {
	getDOM()->setResultUtilization(sumOfProcUtil);
    }
    return *this;
}


/*
 * Print out info for this processor.
 */

std::ostream&
Processor::print( std::ostream& output ) const
{
    const std::ios_base::fmtflags oldFlags = output.setf( std::ios::left, std::ios::adjustfield );
    output << std::setw(8) << name()
	   << " " << std::setw(9) << print_processor_type()
	   << " " << std::setw(5) << replicas()
	   << " " << std::setw(12) << scheduling_label[scheduling()].str
	   << "  "
	   << print_info( *this );	    /* Bonus information about stations -- derived by solver */
    output.flags(oldFlags);
    return output;
}

/* ----------------------- External functions. ------------------------ */

/*
 * Add a processor to the model.
 */

void Processor::create( const std::pair<std::string,LQIO::DOM::Processor*>& p )
{
    const std::string& name = p.first;
    assert( name.size() > 0 );

    if ( Processor::find( name ) ) {
	LQIO::input_error2( LQIO::ERR_DUPLICATE_SYMBOL, "Processor", name.c_str() );
    } else {
	Model::__processor.insert( new Processor( p.second ) );
    }
}



/*
 * Find the processor and return it.
 */

Processor *
Processor::find( const std::string& name )
{
    std::set<Processor *>::const_iterator processor = find_if( Model::__processor.begin(), Model::__processor.end(), EQStr<Entity>( name ) );
    return ( processor != Model::__processor.end() ) ? *processor : NULL;
}


/* static */ std::ostream&
Processor::output_processor_type( std::ostream& output, const Processor& aProcessor )
{
    char buf[12];
    const unsigned n = aProcessor.copies();

    if ( aProcessor.scheduling() == SCHEDULE_CUSTOMER ) {
	sprintf( buf, "ref(%d)", n );
    } else if ( aProcessor.isInfinite() ) {
	sprintf( buf, "inf" );
    } else if ( n > 1 ) {
	sprintf( buf, "mult(%d)", n );
    } else {
	sprintf( buf, "serv" );
    }
    output << buf;
    return output;
}
