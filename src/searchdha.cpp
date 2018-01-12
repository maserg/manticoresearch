//
// $Id$
//

//
// Copyright (c) 2001-2016, Andrew Aksyonoff
// Copyright (c) 2008-2016, Sphinx Technologies Inc
// All rights reserved
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License. You should have
// received a copy of the GPL license along with this program; if you
// did not, you can find it at http://www.gnu.org/
//

#include "sphinx.h"
#include "sphinxstd.h"
#include "sphinxrt.h"
#include "sphinxint.h"
#include <errno.h>

#include "searchdaemon.h"
#include "searchdha.h"

#include <utility>

#if !USE_WINDOWS
	#include <netinet/in.h>
#endif


int				g_iPingInterval		= 0;		// by default ping HA agents every 1 second
DWORD			g_uHAPeriodKarma	= 60;		// by default use the last 1 minute statistic to determine the best HA agent

int				g_iPersistentPoolSize	= 0;

CSphString AgentDesc_c::GetMyUrl() const
{
	CSphString sName;
	switch ( m_iFamily )
	{
	case AF_INET: sName.SetSprintf ( "%s:%u", m_sHost.cstr(), m_iPort ); break;
	case AF_UNIX: sName = m_sPath; break;
	}
	return sName;
}

HostDashboard_t::HostDashboard_t ( const AgentDesc_c & tAgent )
	: m_bNeedPing ( false )
	, m_iErrorsARow ( 0 )
	, m_pPersPool (nullptr)
{
	tAgent.CloneTo ( m_tDescriptor );
	m_iRefCount = 1;
	m_iLastQueryTime = m_iLastAnswerTime = sphMicroTimer () - g_iPingInterval*1000;
	m_dDataLock.Init();
}

HostDashboard_t::~HostDashboard_t ()
{
	m_dDataLock.Done();
	SafeDelete ( m_pPersPool );
}

bool HostDashboard_t::IsOlder ( int64_t iTime ) const
{
	return ( (iTime-m_iLastAnswerTime)>g_iPingInterval*1000LL );
}

DWORD HostDashboard_t::GetCurSeconds()
{
	int64_t iNow = sphMicroTimer()/1000000;
	return DWORD ( iNow & 0xFFFFFFFF );
}

bool HostDashboard_t::IsHalfPeriodChanged ( DWORD * pLast )
{
	assert ( pLast );
	DWORD uSeconds = GetCurSeconds();
	if ( ( uSeconds - *pLast )>( g_uHAPeriodKarma / 2 ) )
	{
		*pLast = uSeconds;
		return true;
	}
	return false;
}

AgentDash_t*	HostDashboard_t::GetCurrentStat()
{
	DWORD uTime = GetCurSeconds()/g_uHAPeriodKarma;
	int iIdx = uTime % STATS_DASH_TIME;
	AgentDash_t & dStats = m_dStats[iIdx];
	if ( dStats.m_uTimestamp!=uTime ) // we have new or reused stat
		dStats.Reset();
	dStats.m_uTimestamp = uTime;
	return &dStats;
}

void HostDashboard_t::GetCollectedStat ( HostStatSnapshot_t& dResult, int iPeriods ) const
{
	AgentDash_t tAccum;
	tAccum.Reset ();

	DWORD uSeconds = GetCurSeconds();
	if ( (uSeconds % g_uHAPeriodKarma) < (g_uHAPeriodKarma/2) )
		++iPeriods;

	iPeriods = Min ( iPeriods, STATS_DASH_TIME );

	DWORD uTime = uSeconds/g_uHAPeriodKarma;
	int iIdx = uTime % STATS_DASH_TIME;

	CSphScopedRLock tRguard ( m_dDataLock );

	for ( ; iPeriods>0 ; --iPeriods )
	{
		const AgentDash_t & dStats = m_dStats[iIdx];
		if ( dStats.m_uTimestamp==uTime ) // it might be no queries at all in the fixed time
			tAccum.Add ( dStats );
		--uTime;
		--iIdx;
		if ( iIdx<0 )
			iIdx = STATS_DASH_TIME-1;
	}
	for ( int i = 0; i<eMaxAgentStat; ++i )
		dResult[i] = tAccum.m_dStats[i].GetValue ();
	for ( int i = 0; i<ehMaxStat; ++i )
		dResult[i+eMaxAgentStat] = tAccum.m_dHostStats[i];
}

PersistentConnectionsPool_t::PersistentConnectionsPool_t ()
	: m_bShutdown (false)
	, m_iRit (0)
	, m_iWit (0)
	, m_iFreeWindow (0)
{}

void PersistentConnectionsPool_t::Init ( int iPoolSize )
{
	CGuard tGuard ( m_dDataLock );
	for ( int i = 0; i<( m_dSockets.GetLength ()-m_iFreeWindow ); ++i )
	{
		int iSock = m_dSockets[Step ( &m_iRit )];
		if ( iSock>=0 )
			sphSockClose ( iSock );
	}
	m_dSockets.Reset ();
	m_dSockets.Reserve ( iPoolSize );
	m_iRit = m_iWit = m_iFreeWindow = 0;
}

// move the iterator to the next item, or loop the ring.
int PersistentConnectionsPool_t::Step ( int* pVar )
{
	assert ( pVar );
	int iRes = *pVar++;
	if ( *pVar>=m_dSockets.GetLength () )
		*pVar = 0;
	return iRes;
}

// Rent first try to return the sockets which already were in work (i.e. which are connected)
// If no such socket available and the limit is not reached, it will add the new one.
int PersistentConnectionsPool_t::RentConnection ()
{
	CGuard tGuard ( m_dDataLock );
	if ( m_iFreeWindow>0 )
	{
		--m_iFreeWindow;
		return m_dSockets[Step ( &m_iRit )];
	}
	if ( m_dSockets.GetLength () == m_dSockets.GetLimit () )
		return -2; // no more slots available;

	// this branch will be executed only during initial 'heating'
	m_dSockets.Add ( -1 );
	return -1;
}

void PersistentConnectionsPool_t::ReturnConnection ( int iSocket )
{
	CGuard tGuard ( m_dDataLock );

	// overloaded pool
	if ( m_iFreeWindow >= m_dSockets.GetLength () )
	{
		// no place at all (i.e. if pool was resized, but some sockets rented before,
		// and now they are returned, but we have no place for them)
		if ( m_dSockets.GetLength () == m_dSockets.GetLimit () )
		{
			sphSockClose ( iSocket );
			return;
		}
		// add the place for one more returned socket
		m_dSockets.Add ();
		m_iWit = m_dSockets.GetLength ()-1;
	}
	++m_iFreeWindow;
	if ( m_bShutdown )
	{
		sphSockClose ( iSocket );
		iSocket = -1;
	}
	// if there were no free sockets until now, point the reading iterator to just released socket
	if ( m_iFreeWindow==1 )
		m_iRit = m_iWit;
	m_dSockets[Step ( &m_iWit )] = iSocket;
}

// close all the sockets in the pool.
void PersistentConnectionsPool_t::Shutdown ()
{
	CGuard tGuard ( m_dDataLock );
	m_bShutdown = true;
	for ( int i = 0; i<( m_dSockets.GetLength ()-m_iFreeWindow ); ++i )
	{
		int& iSock = m_dSockets[Step ( &m_iRit )];
		if ( iSock>=0 )
		{
			sphSockClose ( iSock );
			iSock = -1;
		}
	}
}

void ClosePersistentSockets()
{
	g_tDashes.Lock();
	CSphVector<HostDashboard_t *> dHosts;
	g_tDashes.GetActiveDashes ( dHosts );
	for ( auto& pHost : dHosts )
	{
		if ( pHost->m_pPersPool )
			pHost->m_pPersPool->Shutdown ();
	}
	g_tDashes.Unlock();
}

//////////////////////////////////////////////////////////////////////////
void MultiAgentDesc_t::SetOptions ( const AgentOptions_t& tOpt )
{
	m_eStrategy = tOpt.m_eStrategy;
	for ( auto& dHost : m_dHosts )
	{
		dHost.m_bPersistent = tOpt.m_bPersistent;
		dHost.m_bBlackhole = tOpt.m_bBlackhole;
	}
}

void MultiAgentDesc_t::FinalizeInitialization ()
{
	if ( IsHA () )
	{
		WORD uFrac = WORD ( 0xFFFF / GetLength () );
		m_dWeights.Reset ( GetLength () );
		for ( WORD& uWeight : m_dWeights )
			uWeight = uFrac;
	}
}

const AgentDesc_c & MultiAgentDesc_t::GetAgent ( int iAgent ) const
{
	assert ( iAgent>=0 );
	return m_dHosts[iAgent];
}

AgentDesc_c & MultiAgentDesc_t::GetAgent ( int iAgent )
{
	assert ( iAgent>=0 );
	return m_dHosts[iAgent];
}

bool ValidateAndAddDashboard ( AgentDesc_c * pNewAgent, WarnInfo_t & tInfo );

bool MultiAgentDesc_t::SetHosts ( const CSphVector<AgentDesc_c *> & dHosts, WarnInfo_t & tWarning )
{
	m_dHosts.Reset ( dHosts.GetLength() );
	ARRAY_FOREACH ( i, dHosts )
	{
		dHosts[i]->CloneTo ( m_dHosts[i] );
		if ( !ValidateAndAddDashboard ( m_dHosts.Begin() + i, tWarning ) )
			return false;
	}

	return true;
}

const AgentDesc_c & MultiAgentDesc_t::RRAgent ()
{
	if ( !IsHA() )
		return GetAgent(0);

	int iRRCounter = (int) m_iRRCounter++;
	while ( iRRCounter<0 || iRRCounter> ( GetLength ()-1 ) )
	{
		if ( iRRCounter+1 == (int) m_iRRCounter.CAS ( iRRCounter+1, 1 ) )
			iRRCounter = 0;
		else
			iRRCounter = (int) m_iRRCounter++;
	}

	return GetAgent ( iRRCounter );
}

const AgentDesc_c & MultiAgentDesc_t::RandAgent ()
{
	return GetAgent ( sphRand() % GetLength() );
}

void MultiAgentDesc_t::ChooseWeightedRandAgent ( int * pBestAgent, CSphVector<int> & dCandidates )
{
	assert ( pBestAgent );
	CSphScopedRLock tLock ( m_dWeightLock );
	DWORD uBound = m_dWeights[*pBestAgent];
	DWORD uLimit = uBound;
	for ( auto j : dCandidates )
		uLimit += m_dWeights[j];
	DWORD uChance = sphRand() % uLimit;

	if ( uChance<=uBound )
		return;

	for ( auto j : dCandidates )
	{
		uBound += m_dWeights[j];
		*pBestAgent = j;
		if ( uChance<=uBound )
			break;
	}
}

static void LogAgentWeights ( const WORD * pOldWeights, const WORD * pCurWeights, const int64_t * pTimers, const CSphFixedVector<AgentDesc_c> & dAgents )
{
	if ( g_eLogLevel<SPH_LOG_DEBUG )
		return;

	ARRAY_FOREACH ( i, dAgents )
		sphLogDebug ( "client=%s:%d, mirror=%d, weight=%d, %d, timer=" INT64_FMT, dAgents[i].m_sHost.cstr (), dAgents[i].m_iPort, i, pCurWeights[i], pOldWeights[i], pTimers[i] );
}

const AgentDesc_c & MultiAgentDesc_t::StDiscardDead ()
{
	if ( !IsHA() )
		return GetAgent(0);

	// threshold errors-a-row to be counted as dead
	int iDeadThr = 3;

	int iBestAgent = -1;
	int64_t iErrARow = -1;
	CSphVector<int> dCandidates;
	CSphFixedVector<int64_t> dTimers ( GetLength() );
	dCandidates.Reserve ( GetLength() );

	ARRAY_FOREACH ( i, m_dHosts )
	{
		// no locks for g_pStats since we just reading, and read data is not critical.
		const HostDashboard_t * pDash = m_dHosts[i].m_pDash;

		HostStatSnapshot_t dDashStat;
		pDash->GetCollectedStat (dDashStat);// look at last 30..90 seconds.
		uint64_t uQueries = 0;
		for ( int j=0; j<eMaxAgentStat; ++j )
			uQueries += dDashStat[j];
		if ( uQueries > 0 )
			dTimers[i] = dDashStat[ehTotalMsecs]/uQueries;
		else
			dTimers[i] = 0;

		CSphScopedRLock tRguard ( pDash->m_dDataLock );
		int64_t iThisErrARow = ( pDash->m_iErrorsARow<=iDeadThr ) ? 0 : pDash->m_iErrorsARow;

		if ( iErrARow < 0 )
			iErrARow = iThisErrARow;

		// 2. Among good nodes - select the one(s) with lowest errors/query rating
		if ( iErrARow > iThisErrARow )
		{
			dCandidates.Reset();
			iBestAgent = i;
			iErrARow = iThisErrARow;
		} else if ( iErrARow==iThisErrARow )
		{
			if ( iBestAgent>=0 )
				dCandidates.Add ( iBestAgent );
			iBestAgent = i;
		}
	}

	// check if it is a time to recalculate the agent's weights
	CheckRecalculateWeights ( dTimers );


	// nothing to select, sorry. Just random agent...
	if ( iBestAgent < 0 )
	{
		sphLogDebug ( "HA selector discarded all the candidates and just fall into simple Random" );
		return RandAgent();
	}

	// only one node with lowest error rating. Return it.
	if ( !dCandidates.GetLength() )
	{
		sphLogDebug ( "client=%s:%d, HA selected %d node with best num of errors a row (" INT64_FMT ")", m_dHosts[iBestAgent].m_sHost.cstr(), m_dHosts[iBestAgent].m_iPort, iBestAgent, iErrARow );
		return m_dHosts[iBestAgent];
	}

	// several nodes. Let's select the one.
	ChooseWeightedRandAgent ( &iBestAgent, dCandidates );
	if ( g_eLogLevel>=SPH_LOG_VERBOSE_DEBUG )
	{
		float fAge = 0.0;
		const char * sLogStr = NULL;
		const HostDashboard_t & dDash = GetDashForAgent ( iBestAgent );
		CSphScopedRLock tRguard ( dDash.m_dDataLock );
		fAge = ( dDash.m_iLastAnswerTime-dDash.m_iLastQueryTime ) / 1000.0f;
		sLogStr = "client=%s:%d, HA selected %d node by weighted random, with best EaR (" INT64_FMT "), last answered in %.3f milliseconds, among %d candidates";
		sphLogDebugv ( sLogStr, m_dHosts[iBestAgent].m_sHost.cstr(), m_dHosts[iBestAgent].m_iPort, iBestAgent, iErrARow, fAge, dCandidates.GetLength()+1 );
	}

	return m_dHosts[iBestAgent];
}

// Check the time and recalculate mirror weights, if necessary.
void MultiAgentDesc_t::CheckRecalculateWeights ( const CSphFixedVector<int64_t> & dTimers )
{
	CSphScopedWLock tWguard ( m_dWeightLock );
	if ( dTimers.GetLength () && HostDashboard_t::IsHalfPeriodChanged ( &m_uTimestamp ) )
	{
		CSphFixedVector<WORD> dWeights ( GetLength () );
		// since we'll update values anyway, acquire w-lock.
		memcpy ( dWeights.Begin (), m_dWeights.Begin (), sizeof ( dWeights[0] ) * dWeights.GetLength () );
		RebalanceWeights ( dTimers, dWeights.Begin () );
		LogAgentWeights ( m_dWeights.Begin(), dWeights.Begin (), dTimers.Begin (), m_dHosts );
		memcpy ( m_dWeights.Begin(), dWeights.Begin (), sizeof ( dWeights[0] ) * dWeights.GetLength () );
	}
}

const AgentDesc_c & MultiAgentDesc_t::StLowErrors ()
{
	if ( !IsHA() )
		return GetAgent(0);

	// how much error rating is allowed
	float fAllowedErrorRating = 0.03f; // i.e. 3 errors per 100 queries is still ok

	int iBestAgent = -1;
	float fBestCriticalErrors = 1.0;
	float fBestAllErrors = 1.0;
	CSphVector<int> dCandidates;
	CSphFixedVector<int64_t> dTimers ( m_dHosts.GetLength() );
	dCandidates.Reserve ( m_dHosts.GetLength() );

	ARRAY_FOREACH ( i, m_dHosts )
	{
		// no locks for g_pStats since we just reading, and read data is not critical.
		const HostDashboard_t & dDash = GetDashForAgent ( i );

		HostStatSnapshot_t dDashStat;
		dDash.GetCollectedStat (dDashStat); // look at last 30..90 seconds.
		uint64_t uQueries = 0;
		uint64_t uCriticalErrors = 0;
		uint64_t uAllErrors = 0;
		uint64_t uSuccesses = 0;
		for ( int j=0; j<eMaxAgentStat; ++j )
		{
			if ( j==eNetworkCritical )
				uCriticalErrors = uQueries;
			else if ( j==eNetworkNonCritical )
			{
				uAllErrors = uQueries;
				uSuccesses = dDashStat[j];
			}
			uQueries += dDashStat[j];
		}

		if ( uQueries > 0 )
			dTimers[i] = dDashStat[ehTotalMsecs]/uQueries;
		else
			dTimers[i] = 0;

		// 1. No successes queries last period (it includes the pings). Skip such node!
		if ( !uSuccesses )
			continue;

		if ( uQueries )
		{
			// 2. Among good nodes - select the one(s) with lowest errors/query rating
			float fCriticalErrors = (float) uCriticalErrors/uQueries;
			float fAllErrors = (float) uAllErrors/uQueries;
			if ( fCriticalErrors<=fAllowedErrorRating )
				fCriticalErrors = 0.0f;
			if ( fAllErrors<=fAllowedErrorRating )
				fAllErrors = 0.0f;
			if ( fCriticalErrors < fBestCriticalErrors )
			{
				dCandidates.Reset();
				iBestAgent = i;
				fBestCriticalErrors = fCriticalErrors;
				fBestAllErrors = fAllErrors;
			} else if ( fCriticalErrors==fBestCriticalErrors )
			{
				if ( fAllErrors < fBestAllErrors )
				{
					dCandidates.Reset();
					iBestAgent = i;
					fBestAllErrors = fAllErrors;
				} else if ( fAllErrors==fBestAllErrors )
				{
					if ( iBestAgent>=0 )
						dCandidates.Add ( iBestAgent );
					iBestAgent = i;
				}
			}
		}
	}

	// check if it is a time to recalculate the agent's weights
	CheckRecalculateWeights ( dTimers );

	// nothing to select, sorry. Just plain RR...
	if ( iBestAgent < 0 )
	{
		sphLogDebug ( "HA selector discarded all the candidates and just fall into simple Random" );
		return RandAgent();
	}

	// only one node with lowest error rating. Return it.
	if ( !dCandidates.GetLength() )
	{
		sphLogDebug ( "client=%s:%d, HA selected %d node with best error rating (%.2f)", m_dHosts[iBestAgent].m_sHost.cstr(), m_dHosts[iBestAgent].m_iPort, iBestAgent, fBestCriticalErrors );
		return m_dHosts[iBestAgent];
	}

	// several nodes. Let's select the one.
	ChooseWeightedRandAgent ( &iBestAgent, dCandidates );
	if ( g_eLogLevel>=SPH_LOG_VERBOSE_DEBUG )
	{
		float fAge = 0.0f;
		const char * sLogStr = NULL;
		const HostDashboard_t & dDash = GetDashForAgent ( iBestAgent );
		CSphScopedRLock tRguard ( dDash.m_dDataLock );
		fAge = ( dDash.m_iLastAnswerTime-dDash.m_iLastQueryTime ) / 1000.0f;
		sLogStr = "client=%s:%d, HA selected %d node by weighted random, with best error rating (%.2f), answered %f seconds ago";
		sphLogDebugv ( sLogStr, m_dHosts[iBestAgent].m_sHost.cstr(), m_dHosts[iBestAgent].m_iPort, iBestAgent, fBestCriticalErrors, fAge );
	}

	return m_dHosts[iBestAgent];
}


const AgentDesc_c & MultiAgentDesc_t::ChooseAgent ()
{
	switch ( m_eStrategy )
	{
	case HA_AVOIDDEAD:
		return StDiscardDead();
	case HA_AVOIDERRORS:
		return StLowErrors();
	case HA_ROUNDROBIN:
		return RRAgent();
	default:
		return RandAgent();
	}
}


void MultiAgentDesc_t::QueuePings()
{
	if ( !IsHA() )
		return;

	ARRAY_FOREACH ( i, m_dHosts )
		m_dHosts[i].m_pDash->m_bNeedPing = true;
}

//////////////////////////////////////////////////////////////////////////
AgentConn_t::AgentConn_t ()
	: m_iSock ( -1 )
	, m_bFresh ( true )
	, m_bSuccess ( false )
	, m_bDone ( false )
	, m_eReplyStatus ( SEARCHD_ERROR )
	, m_iReplySize ( 0 )
	, m_iReplyRead ( 0 )
	, m_iRetries ( 0 )
	, m_iRetryLimit ( 0 )
	, m_dReplyBuf ( 0 )
	, m_iWall ( 0 )
	, m_iWaited ( 0 )
	, m_iStartQuery ( 0 )
	, m_iEndQuery ( 0 )
	, m_iWorkerTag ( -1 )
	, m_iStoreTag ( 0 )
	, m_iWeight ( -1 )
	, m_bPing ( false )
	, m_iMirror ( 0 )
	, m_iMirrorsCount ( 1 )
	, m_eConnState ( AGENT_UNUSED )
{}

AgentConn_t::~AgentConn_t ()
{
	Close ( false );
	if ( m_tDesc.m_bPersistent && m_tDesc.m_pDash->m_pPersPool )
		m_tDesc.m_pDash->m_pPersPool->ReturnConnection ( m_iSock );
}

void AgentConn_t::State ( AgentState_e eState )
{
	sphLogDebugv ( "state %d > %d, sock %d, order %d, %p", m_eConnState, eState, m_iSock, m_iStoreTag, this );
	m_eConnState = eState;
}

void AgentConn_t::Close ( bool bClosePersist )
{
	m_dReplyBuf.Reset ( 0 );
	if ( m_iSock>0 )
	{
		m_bFresh = false;
		if ( ( m_tDesc.m_bPersistent && bClosePersist ) || !m_tDesc.m_bPersistent )
		{
			sphSockClose ( m_iSock );
			m_iSock = -1;
			m_bFresh = true;
		}
		if ( State()!=AGENT_RETRY )
			State ( AGENT_UNUSED );
	}
	m_iWall += sphMicroTimer ();
}

void AgentConn_t::SetMirror ( const CSphString & sIndex, int iAgent, const AgentDesc_c & tDesc, int iMirrorsCount )
{
	tDesc.CloneTo ( m_tDesc );
	m_sDistIndex = sIndex;
	m_iMirror = iAgent;
	m_iMirrorsCount = iMirrorsCount;
	SetupPersist();
}

void AgentConn_t::SetupPersist()
{
	if ( m_tDesc.m_bPersistent && m_tDesc.m_pDash->m_pPersPool )
	{
		m_iSock = m_tDesc.m_pDash->m_pPersPool->RentConnection ();
		if ( m_iSock==-2 ) // no free persistent connections. This connection will be not persistent
			m_tDesc.m_bPersistent = false;
	}
	m_bFresh = ( m_tDesc.m_bPersistent && m_iSock<0 );
}


SearchdStats_t			g_tStats;
cDashStorage			g_tDashes;


// generic stats track - always to agent stats, separately to dashboard.
void agent_stats_inc ( AgentConn_t & tAgent, AgentStats_e iCounter )
{
	assert ( iCounter<=eMaxAgentStat );
	assert ( tAgent.m_tDesc.m_pDash );

	if ( tAgent.m_tDesc.m_pStats )
		++tAgent.m_tDesc.m_pStats->m_dStats[iCounter];

	HostDashboard_t & tIndexDash = *tAgent.m_tDesc.m_pDash;
	CSphScopedWLock tWguard ( tIndexDash.m_dDataLock );
	AgentDash_t & tAgentDash = *tIndexDash.GetCurrentStat ();
	++tAgentDash.m_dStats[iCounter];
	if ( iCounter>=eNetworkNonCritical && iCounter<eMaxAgentStat )
		tIndexDash.m_iErrorsARow = 0;
	else
		++tIndexDash.m_iErrorsARow;

	tAgent.m_iEndQuery = sphMicroTimer();
	tIndexDash.m_iLastQueryTime = tAgent.m_iStartQuery;
	tIndexDash.m_iLastAnswerTime = tAgent.m_iEndQuery;
	// do not count query time for pings
	// only count errors
	if ( !tAgent.m_bPing )
	{
		tAgentDash.m_dHostStats[ehTotalMsecs]+=tAgent.m_iEndQuery-tAgent.m_iStartQuery;
		if ( tAgent.m_tDesc.m_pStats )
			tAgent.m_tDesc.m_pStats->m_dHostStats[ehTotalMsecs] += tAgent.m_iEndQuery - tAgent.m_iStartQuery;
	}
}

// special case of stats - all is ok, just need to track the time in dashboard.
void track_processing_time ( AgentConn_t & tAgent )
{
	// first we count temporary statistic (into dashboard)
	assert ( tAgent.m_tDesc.m_pDash );
	CSphScopedWLock tWguard ( tAgent.m_tDesc.m_pDash->m_dDataLock );
	uint64_t* pCurStat = tAgent.m_tDesc.m_pDash->GetCurrentStat ()->m_dHostStats;
	uint64_t uConnTime = (uint64_t) sphMicroTimer () - tAgent.m_iStartQuery;

	++pCurStat[ehConnTries];
	if ( uint64_t ( uConnTime )>pCurStat[ehMaxMsecs] )
		pCurStat[ehMaxMsecs] = uConnTime;

	if ( pCurStat[ehConnTries]>1 )
		pCurStat[ehAverageMsecs] = ( pCurStat[ehAverageMsecs]*( pCurStat[ehConnTries]-1 )+uConnTime )/pCurStat[ehConnTries];
	else
		pCurStat[ehAverageMsecs] = uConnTime;

	// then we count permanent statistic (for show status)
	if ( tAgent.m_tDesc.m_pStats )
	{
		uint64_t * pHStat = tAgent.m_tDesc.m_pStats->m_dHostStats;
		++pHStat[ehConnTries];
		if ( uint64_t ( uConnTime )>pHStat[ehMaxMsecs] )
			pHStat[ehMaxMsecs] = uConnTime;
		if ( pHStat[ehConnTries]>1 )
			pHStat[ehAverageMsecs] = ( pHStat[ehAverageMsecs] * ( pHStat[ehConnTries] - 1 ) + uConnTime ) / pHStat[ehConnTries];
		else
			pHStat[ehAverageMsecs] = uConnTime;
	}
}

// try to parse hostname/ip/port or unixsocket on current pConfigLine.
// fill pAgent fields on success and move ppLine pointer next after parsed instance
bool ParseAddressPort ( AgentDesc_c * pAgent, const char ** ppLine, const WarnInfo_t &dInfo, bool bUseDefaultPort )
{
	// extract host name or path
	const char *&p = *ppLine;
	const char * pAnchor = p;

	if ( !p )
		return false;

#if !USE_WINDOWS
	enum AddressType_e
	{ apIP, apUNIX };
	AddressType_e eState = apIP;
#endif
	if ( *p=='/' ) // whether we parse inet or unix socket
	{
#if USE_WINDOWS
		sphWarning ( "index '%s': agent '%s': UNIX sockets are not supported on Windows - SKIPPING AGENT",
			dInfo.m_szIndexName, dInfo.m_szAgent );
		return false;
#else
		eState = apUNIX;
#endif
	}

	while ( sphIsAlpha ( *p ) || *p=='.' || *p=='-' || *p=='/' )
		++p;
	if ( p==pAnchor )
	{
		sphWarning ( "index '%s': agent '%s': host name or path expected - SKIPPING AGENT", dInfo.m_szIndexName,
			dInfo.m_szAgent );
		return false;
	}

	CSphString sSub ( pAnchor, p-pAnchor );
#if !USE_WINDOWS
	if ( eState==apUNIX )
	{
		if ( strlen ( sSub.cstr () ) + 1>sizeof(((struct sockaddr_un *)0)->sun_path) )
		{
			sphWarning ( "index '%s': agent '%s': UNIX socket path is too long - SKIPPING AGENT", dInfo.m_szIndexName,
				 dInfo.m_szAgent );
			return false;
		}

		pAgent->m_iFamily = AF_UNIX;
		pAgent->m_sPath = sSub;
		return true;
	}
#endif
	// below is only deal with inet sockets
	pAgent->m_iFamily = AF_INET;
	pAgent->m_sHost = sSub;

	// expect ':' (and then portnum) after address
	if ( *p!=':' )
	{
		if ( bUseDefaultPort )
		{
			pAgent->m_iPort = IANA_PORT_SPHINXAPI;
			sphWarning ( "index '%s': agent '%s': colon and portnum expected before '%s' - Using default IANA %d port", dInfo.m_szIndexName,
				 dInfo.m_szAgent, p, pAgent->m_iPort );
			return true;
		} else
		{
			sphWarning ( "index '%s': agent '%s': colon expected before '%s' - SKIPPING AGENT", dInfo.m_szIndexName
						 , dInfo.m_szAgent, p );
			return false;
		}
	}
	pAnchor = ++p;

	// parse portnum
	while ( isdigit(*p) )
		++p;

	if ( p==pAnchor )
	{
		if ( bUseDefaultPort )
		{
			pAgent->m_iPort = IANA_PORT_SPHINXAPI;
			sphWarning ( "index '%s': agent '%s': portnum expected before '%s' - Using default IANA %d port"
						 , dInfo.m_szIndexName, dInfo.m_szAgent, p, pAgent->m_iPort );
			--p; /// step back to ':'
			return true;
		} else
		{
			sphWarning ( "index '%s': agent '%s': port number expected before '%s' - SKIPPING AGENT"
						 , dInfo.m_szIndexName, dInfo.m_szAgent, p );
			return false;
		}
	}
	pAgent->m_iPort = atoi ( pAnchor );

	if ( !IsPortInRange ( pAgent->m_iPort ) )
	{
		sphWarning ( "index '%s': agent '%s': invalid port number near '%s' - SKIPPING AGENT", dInfo.m_szIndexName,
			dInfo.m_szAgent, p );
		return false;

	}
	return true;
}

void AgentConn_t::Fail ( AgentStats_e eStat, const char* sMessage, ... )
{
	State ( AGENT_RETRY );
	Close ();
	va_list ap;
	va_start ( ap, sMessage );
	m_sFailure.SetSprintfVa ( sMessage, ap );
	va_end ( ap );
	agent_stats_inc ( *this, eStat );
}

#define sphStrMatchStatic(_str, _cstr) ( strncmp ( _str, _cstr, sizeof(_str)-1 )==0 )

bool ParseStrategyHA ( const char * sName, HAStrategies_e & eStrategy )
{
	if ( sphStrMatchStatic ( "random", sName ) )
		eStrategy = HA_RANDOM;
	else if ( sphStrMatchStatic ( "roundrobin", sName ) )
		eStrategy = HA_ROUNDROBIN;
	else if ( sphStrMatchStatic ( "nodeads", sName ) )
		eStrategy = HA_AVOIDDEAD;
	else if ( sphStrMatchStatic ( "noerrors", sName ) )
		eStrategy = HA_AVOIDERRORS;
	else
		return false;

	return true;
}

static bool IsAgentDelimiter ( char c )
{
	return c=='|' || c=='[' || c==']';
}


bool ValidateAndAddDashboard ( AgentDesc_c * pNewAgent, WarnInfo_t & tInfo )
{
	assert ( pNewAgent );
	CSphString sPrefix;
	assert ( tInfo.m_szAgent );
	if ( tInfo.m_szIndexName )
		sPrefix.SetSprintf ( "index '%s': agent '%s':", tInfo.m_szIndexName, tInfo.m_szAgent );
	else
		sPrefix.SetSprintf ( "host '%s':", tInfo.m_szAgent );

	// lookup address (if needed)
	if ( pNewAgent->m_iFamily==AF_INET )
	{
		if ( pNewAgent->m_sHost.IsEmpty () )
		{
			sphWarning ( "%s invalid host name 'empty' - SKIPPING AGENT", sPrefix.cstr() );
			return false;
		}

		pNewAgent->m_uAddr = sphGetAddress ( pNewAgent->m_sHost.cstr () );
		if ( pNewAgent->m_uAddr==0 )
		{
			sphWarning ( "%s failed to lookup host name '%s' (error=%s) - SKIPPING AGENT",
				sPrefix.cstr (), pNewAgent->m_sHost.cstr (), sphSockError () );
			return false;
		}
	}

	pNewAgent->m_pStats = new AgentDash_t;
	g_tDashes.AddAgent ( pNewAgent );

	assert ( pNewAgent->m_pStats );
	assert ( pNewAgent->m_pDash );
	return true;
}

// convert all 'host mirrors' (i.e. agents without indices) into 'index mirrors'
// it allows to write agents as 'host1|host2|host3:index' instead of 'host1:index|host2:index|host3:index'.
void FixupOrphanedAgents ( MultiAgentDesc_t & tMultiAgent )
{
	if ( !tMultiAgent.IsHA () )
		return;

	CSphString sIndexes;
	for ( int i = tMultiAgent.GetLength ()-1; i>=0; --i )
	{
		AgentDesc_c & tMyAgent = tMultiAgent.GetAgent ( i );
		if ( tMyAgent.m_sIndexes.IsEmpty () )
		{
			tMyAgent.m_sIndexes = sIndexes;
			if ( sIndexes.IsEmpty () )
				sphWarning ( "Agent %s in list specified without index(es) it serves!", tMyAgent.GetMyUrl().cstr() );
		} else
			sIndexes = tMyAgent.m_sIndexes;
	}
}

bool ConfigureAgent ( MultiAgentDesc_t & tAgent, const char * szAgent, const char * szIndexName, AgentOptions_t tDesc )
{
	enum AgentParse_e { AP_WANT_ADDRESS, AP_OPTIONS, AP_DONE };
	AgentParse_e eState = AP_DONE;
	VectorPtrsGuard_T<AgentDesc_c> tDescGuard;
	CSphVector<AgentDesc_c *> & dHosts = tDescGuard.m_dPtrs;
	AgentDesc_c * pNewAgent = new AgentDesc_c();
	dHosts.Add ( pNewAgent );
	WarnInfo_t dWI ( szIndexName, szAgent );

	// extract host name or path
	const char * p = szAgent;
	const char * pAnchor;

	while ( *p && isspace ( *p ) )
		++p;

	// might be agent options at head
	if ( *p=='[' )
	{
		eState = AP_OPTIONS;
		++p;
	} else if ( *p )
		eState = AP_WANT_ADDRESS;

	while ( eState!=AP_DONE )
	{
		switch ( eState )
		{
		case AP_WANT_ADDRESS:
			{
				if ( !ParseAddressPort ( pNewAgent, &p, dWI, true ) )
					return false;
			}

			if ( !*p )
			{
				eState = AP_DONE;
				break;
			}

			if ( IsAgentDelimiter ( *p ) )
			{
				eState = ( *p=='|' ? AP_WANT_ADDRESS : AP_OPTIONS );
				pNewAgent = new AgentDesc_c();
				dHosts.Add ( pNewAgent );
				++p;
				break;
			}

			if ( *p!=':' )
			{
				sphWarning ( "index '%s': agent '%s': colon expected near '%s' - SKIPPING AGENT",
					dWI.m_szIndexName, dWI.m_szAgent, p );
				return false;
			}

			++p; // step after ':'
			while ( isspace ( *p ) )
				++p;

			pAnchor = p;

			while ( sphIsAlpha(*p) || isspace(*p) || *p==',' )
				++p;

			if ( *p && !IsAgentDelimiter ( *p ) )
			{
				sphWarning ( "index '%s': agent '%s': index list expected near '%s' - SKIPPING AGENT",
					dWI.m_szIndexName, dWI.m_szAgent, p );
				return false;
			}
			pNewAgent->m_sIndexes = CSphString ( pAnchor, p-pAnchor);

			if ( IsAgentDelimiter ( *p ) )
			{
				if ( *p=='|' )
				{
					++p;
					eState = AP_WANT_ADDRESS;
					pNewAgent = new AgentDesc_c();
					dHosts.Add ( pNewAgent );
				} else
					eState = AP_OPTIONS;
				break;
			} else
				eState = AP_DONE;

			break;

		case AP_OPTIONS:
			{
				const char * sOptName = NULL;
				const char * sOptValue = NULL;
				bool bGotEq = false;
				while ( *p )
				{
					bool bNextOpt = ( *p==',' );
					bool bNextAgent = IsAgentDelimiter ( *p );
					bGotEq |= ( *p=='=' );

					if ( bNextOpt || bNextAgent )
					{
						if ( sOptName && sOptValue )
						{
							bool bParsed = false;
							if ( sphStrMatchStatic ( "conn", sOptName ) )
							{
								if ( sphStrMatchStatic ( "pconn", sOptValue ) || sphStrMatchStatic ( "persistent", sOptValue ) )
								{
									tDesc.m_bPersistent = true;
									bParsed = true;
								}
							} else if ( sphStrMatchStatic ( "ha_strategy", sOptName ) )
							{
								bParsed = ParseStrategyHA ( sOptValue, tDesc.m_eStrategy );
							} else if ( sphStrMatchStatic ( "blackhole", sOptName ) )
							{
								tDesc.m_bBlackhole = ( atoi ( sOptValue )!=0 );
								bParsed = true;
							}

							if ( !bParsed )
							{
								CSphString sInvalid;
								sInvalid.SetBinary ( sOptName, p-sOptName );
								sphWarning ( "index '%s': agent '%s': unknown agent option '%s' ", szIndexName, szAgent, sInvalid.cstr() );
							}
						}

						sOptName = sOptValue = NULL;
						bGotEq = false;
						if ( bNextAgent )
							break;
					}

					if ( sphIsAlpha ( *p ) )
					{
						if ( !sOptName )
							sOptName = p;
						else if ( bGotEq && !sOptValue )
							sOptValue = p;
					}
					++p;
				}
				if ( IsAgentDelimiter ( *p ) )
				{
					eState = AP_WANT_ADDRESS;
					++p;
				} else
					eState = AP_DONE;
			}
			break;

//		case AP_DONE: // AP_DONE catched by while() condition
		default:;
		} // switch (eState)
	} // while (eState!=AP_DONE)

	if ( pNewAgent->m_sIndexes.IsEmpty () )
	{
		pNewAgent->m_sIndexes = szIndexName;
		sphWarning ( "index '%s': agent '%s': no index name(s) defined. Assuming name of the current index ('%s') "
					 , szIndexName, szAgent, szIndexName );
	}

	bool bRes = tAgent.SetHosts ( dHosts, dWI );
	FixupOrphanedAgents ( tAgent );
	tAgent.SetOptions ( tDesc );
	tAgent.FinalizeInitialization ();
	tAgent.QueuePings();
	return bRes;
}

#undef sphStrMatchStatic

AgentDesc_c::~AgentDesc_c ()
{
	SafeRelease ( m_pDash );
	SafeRelease ( m_pStats );
}

void AgentDesc_c::CloneTo ( AgentDesc_c & tOther ) const
{
	SafeRelease ( tOther.m_pDash );
	SafeRelease ( tOther.m_pStats );

	tOther.m_pDash = m_pDash;
	tOther.m_pStats = m_pStats;
	if ( tOther.m_pDash )
		tOther.m_pDash->AddRef();
	if ( tOther.m_pStats )
		tOther.m_pStats->AddRef();

	tOther.m_sIndexes = m_sIndexes;
	tOther.m_bBlackhole = m_bBlackhole;
	tOther.m_uAddr = m_uAddr;
	tOther.m_bPersistent = m_bPersistent;
	tOther.m_iFamily = m_iFamily;
	tOther.m_sPath = m_sPath;
	tOther.m_sHost = m_sHost;
	tOther.m_iPort = m_iPort;
}

void cDashStorage::AddAgent ( AgentDesc_c * pNewAgent )
{
	CSphScopedWLock tWguard ( m_tLock );
	bool bFound = false;
	ARRAY_FOREACH ( i, m_dDashes )
	{
		if ( m_dDashes[i]->GetRefcount ()==1 )
		{
			SafeRelease ( m_dDashes[i] );
			m_dDashes.RemoveFast ( i );
		} else if ( !bFound && pNewAgent->GetMyUrl ()==m_dDashes[i]->m_tDescriptor.GetMyUrl () )
		{
			m_dDashes[i]->AddRef();
			pNewAgent->m_pDash = m_dDashes[i];
			bFound = true;
		}
	}
	if ( !bFound )
	{
		pNewAgent->m_pDash = new HostDashboard_t ( *pNewAgent );
		pNewAgent->m_pDash->AddRef();
		assert (pNewAgent->m_pDash->GetRefcount ()==2);
		m_dDashes.Add ( pNewAgent->m_pDash );
	}
}

// Due to very rare template of usage, linear search is quite enough here
HostDashboard_t * cDashStorage::FindAgent ( const char* sAgent ) const
{
	CSphScopedRLock tRguard ( m_tLock );
	ARRAY_FOREACH ( i, m_dDashes )
	{
		if ( m_dDashes[i]->GetRefcount()==1 )
			continue;
		
		if ( m_dDashes[i]->m_tDescriptor.GetMyUrl () == sAgent )
			return m_dDashes[i];
	}
	return nullptr; // not found
}

void cDashStorage::GetActiveDashes ( CSphVector<HostDashboard_t *> & dAgents ) const
{
	dAgents.Reset ();
	CSphScopedRLock tRguard ( m_tLock );
	for ( auto pDash : m_dDashes )
	{
		if ( pDash->GetRefcount()==1 )
			continue;
		dAgents.Add ( pDash );
	}
}

// let's move some stuff into small functions.
// every of them performs small task and returns false on any critical fail.

// send sphinx_version to remote host. Move to AGENT_HANDSHAKE state
inline static bool SendHandshake ( AgentConn_t &tAgent, ISphNetEvents * pEvents=nullptr )
{
	// send the client's proto version right now to avoid w-w-r pattern.
	NetOutputBuffer_c tOut ( tAgent.m_iSock );
	tOut.SendDword ( SPHINX_CLIENT_VERSION );
	tOut.Flush ();
	if ( tOut.GetError () )
	{
		if ( pEvents ) pEvents->IterateRemove ( tAgent.m_iSock );
		tAgent.Fail ( eNetworkErrors, "sending client_version: %s", tOut.GetErrorMsg () );
		return false;
	}
	if ( pEvents ) pEvents->IterateChangeEvent ( tAgent.m_iSock, ISphNetEvents::SPH_POLL_RD );
	return true;
}

// send 'command-persist' to remote host, if necessary
inline static bool SetRemotePersistent ( AgentConn_t &tAgent, ISphNetEvents * pEvents )
{
	NetOutputBuffer_c tOut ( tAgent.m_iSock );
	// check if we need to reset the persistent connection
	if ( tAgent.m_bFresh && tAgent.m_tDesc.m_bPersistent )
	{
		tOut.SendWord ( SEARCHD_COMMAND_PERSIST );
		tOut.SendWord ( 0 ); // dummy version
		tOut.SendInt ( 4 ); // request body length
		tOut.SendInt ( 1 ); // set persistent to 1.
		tOut.Flush ();
		if ( tOut.GetError () )
		{
			pEvents->IterateRemove ( tAgent.m_iSock );
			tAgent.Fail ( eNetworkErrors, "sending command_persist: %s", tOut.GetErrorMsg () );
			return false;
		}
		tAgent.m_bFresh = false;
	}
	pEvents->IterateChangeEvent ( tAgent.m_iSock, ISphNetEvents::SPH_POLL_WR );
	return true;
}

// receive and check answer to previously issued sphinx_client_version. Move to AGENT_ESTABLISHED state
inline static bool CheckRemoteVer ( AgentConn_t &tAgent, ISphNetEvents * pEvents )
{
	assert ( pEvents );
	// read reply
	int iRemoteVer;
	int iRes = sphSockRecv ( tAgent.m_iSock, ( char * ) &iRemoteVer, sizeof ( iRemoteVer ) );
	if ( iRes!=sizeof ( iRemoteVer ) )
	{
		pEvents->IterateRemove ( tAgent.m_iSock );
		if ( iRes<0 )
		{
			// network error
			int iErr = sphSockGetErrno ();
			tAgent.Fail ( eNetworkErrors, "handshake failure (errno=%d, msg=%s)", iErr, sphSockError ( iErr ) );
		} else if ( iRes>0 )
		{
			// incomplete reply
			tAgent.Fail ( eWrongReplies, "handshake failure (exp=%d, recv=%d)", ( int ) sizeof ( iRemoteVer ), iRes );
		} else
		{
			// agent closed the connection
			// this might happen in out-of-sync connect-accept case; so let's retry
			tAgent.Fail ( eUnexpectedClose, "handshake failure (received %d from 4 bytes, then connection was closed)"
						  , iRes );
			// tAgent.m_eState = AGENT_RETRY; // commented out since it is done in Fail()
		}
		return false;
	}

	iRemoteVer = ntohl ( iRemoteVer );
	if ( !( iRemoteVer==SPHINX_SEARCHD_PROTO
		|| iRemoteVer==0x01000000UL ) ) // workaround for all the revisions that sent it in host order...
	{
		pEvents->IterateRemove ( tAgent.m_iSock );
		tAgent.Fail ( eWrongReplies, "handshake failure (unexpected protocol version=%d)", iRemoteVer );
		return false;
	}
	return SetRemotePersistent ( tAgent, pEvents );
}

// actually build and send request. Move to AGENT_QUERYED state
inline static bool SendRequest ( AgentConn_t &tAgent, const IRequestBuilder_t* pBuilder, ISphNetEvents * pEvents )
{
	assert ( pBuilder );
	// send request
	NetOutputBuffer_c tOut ( tAgent.m_iSock );
	pBuilder->BuildRequest ( tAgent, tOut );
	tOut.Flush ();
	if ( tOut.GetError () )
	{
		pEvents->IterateRemove (tAgent.m_iSock);
		tAgent.Fail ( eNetworkErrors, "sending request %s", tOut.GetErrorMsg () );
		return false;
	}
	pEvents->IterateChangeEvent (tAgent.m_iSock, ISphNetEvents::SPH_POLL_RD);
	return true;
}

// Try to read header and allocate reply buffer. Move to AGENT_REPLY state
inline static bool CheckReplyHeader ( AgentConn_t & tAgent, ISphNetEvents * pEvents )
{
	// try to read
	struct
	{
		WORD m_uStatus;
		WORD m_uVer;
		int m_iLength;
	} tReplyHeader ;

	STATIC_SIZE_ASSERT ( tReplyHeader, 8 );

	if ( sphSockRecv ( tAgent.m_iSock, ( char * ) &tReplyHeader, sizeof ( tReplyHeader ) )!=sizeof ( tReplyHeader ) )
	{
		// bail out if failed
		pEvents->IterateRemove ( tAgent.m_iSock );
		tAgent.Fail ( eNetworkErrors, "failed to receive reply header" );
		return false;
	}

	tReplyHeader.m_uStatus = ntohs ( tReplyHeader.m_uStatus );
	tReplyHeader.m_uVer = ntohs ( tReplyHeader.m_uVer );
	tReplyHeader.m_iLength = ntohl ( tReplyHeader.m_iLength );

	// check the packet
	if ( tReplyHeader.m_iLength<0
		|| tReplyHeader.m_iLength>g_iMaxPacketSize ) // FIXME! add reasonable max packet len too
	{
		pEvents->IterateRemove ( tAgent.m_iSock );
		tAgent.Fail ( eWrongReplies, "invalid packet size (status=%d, len=%d, max_packet_size=%d)"
									   , tReplyHeader.m_uStatus, tReplyHeader.m_iLength, g_iMaxPacketSize );
		return false;
	}

	// header received, switch the status
	assert ( !tAgent.m_dReplyBuf.Begin () );
	tAgent.m_dReplyBuf.Reset ( tReplyHeader.m_iLength );
	if ( !tAgent.m_dReplyBuf.Begin () )
	{
		// bail out if failed
		pEvents->IterateRemove ( tAgent.m_iSock );
		tAgent.Fail ( eWrongReplies, "failed to alloc %d bytes for reply buffer", tAgent.m_iReplySize );
		return false;
	}

	tAgent.m_iReplySize = tReplyHeader.m_iLength;
	tAgent.m_iReplyRead = 0;
	tAgent.m_eReplyStatus = ( SearchdStatus_e ) tReplyHeader.m_uStatus;
	return true;
}

// receive sequence BLOB of iReplySize. Move tAgent.m_iReplyRead.
inline static bool GetReplyChunk ( AgentConn_t &tAgent, ISphNetEvents * pEvents )
{
	assert ( tAgent.m_iReplyRead<tAgent.m_iReplySize );
	int iRes =sphSockRecv ( tAgent.m_iSock, ( char * ) tAgent.m_dReplyBuf.Begin () + tAgent.m_iReplyRead,
		tAgent.m_iReplySize - tAgent.m_iReplyRead );

	// bail out if read failed
	if ( iRes<=0 )
	{
		pEvents->IterateRemove ( tAgent.m_iSock );
		tAgent.Fail ( eNetworkErrors, "failed to receive reply body: %s", sphSockError () );
		return false;
	}

	assert ( iRes>0 );
	assert ( tAgent.m_iReplyRead + iRes<=tAgent.m_iReplySize );
	tAgent.m_iReplyRead += iRes;
	return true;
};


inline static void AgentFailure ( AgentConn_t & tAgent )
{
	tAgent.Close ();
	tAgent.m_dResults.Reset ();
}

inline static bool NonQueryCond ( AgentState_e eCondition )
{
	return eCondition<AGENT_CONNECTING || eCondition>AGENT_QUERYED;
}

inline static bool InWaitCond ( AgentState_e eCondition )
{
	return eCondition==AGENT_QUERYED || eCondition==AGENT_PREREPLY || eCondition==AGENT_REPLY;
}


// change state of new agents to AGENT_CONNECTING or AGENT_HANDSHAKE.
// change state of already connected to AGENT_ESTABLISHED
// change state of fails to AGENT_RETRY
void RemoteConnectToAgent ( AgentConn_t & tAgent )
{
	bool bAgentRetry = ( tAgent.State()==AGENT_RETRY );
	tAgent.State ( AGENT_UNUSED );

	if ( tAgent.m_iSock>=0 ) // already connected
	{
		if ( !sphNBSockEof ( tAgent.m_iSock ) )
		{
			tAgent.State ( AGENT_ESTABLISHED );
			tAgent.m_iStartQuery = sphMicroTimer();
			tAgent.m_iWall -= tAgent.m_iStartQuery;
			return;
		}
		tAgent.Close();
	}

	tAgent.m_bSuccess = false;

	socklen_t len = 0;
	struct sockaddr_storage ss;
	memset ( &ss, 0, sizeof(ss) );
	ss.ss_family = (short)tAgent.m_tDesc.m_iFamily;

	if ( ss.ss_family==AF_INET )
	{
		DWORD uAddr = tAgent.m_tDesc.m_uAddr;
		if ( g_bHostnameLookup && !tAgent.m_tDesc.m_sHost.IsEmpty() )
		{
			DWORD uRenew = sphGetAddress ( tAgent.m_tDesc.m_sHost.cstr(), false );
			if ( uRenew )
				uAddr = uRenew;
		}

		struct sockaddr_in *in = (struct sockaddr_in *)&ss;
		in->sin_port = htons ( (unsigned short)tAgent.m_tDesc.m_iPort );
		in->sin_addr.s_addr = uAddr;
		len = sizeof(*in);
	}
#if !USE_WINDOWS
	else if ( ss.ss_family==AF_UNIX )
	{
		struct sockaddr_un *un = (struct sockaddr_un *)&ss;
		snprintf ( un->sun_path, sizeof(un->sun_path), "%s", tAgent.m_tDesc.m_sPath.cstr() );
		len = sizeof(*un);
	}
#endif

	tAgent.m_iSock = socket ( tAgent.m_tDesc.m_iFamily, SOCK_STREAM, 0 );
	if ( tAgent.m_iSock<0 )
	{
		tAgent.m_sFailure.SetSprintf ( "socket() failed: %s", sphSockError() );
		return;
	}

	if ( sphSetSockNB ( tAgent.m_iSock )<0 )
	{
		tAgent.m_sFailure.SetSprintf ( "sphSetSockNB() failed: %s", sphSockError() );
		return;
	}

#ifdef TCP_NODELAY
	int iOn = 1;
	if ( ss.ss_family==AF_INET && setsockopt ( tAgent.m_iSock, IPPROTO_TCP, TCP_NODELAY, (char*)&iOn, sizeof(iOn) ) )
	{
		tAgent.m_sFailure.SetSprintf ( "setsockopt() failed: %s", sphSockError() );
		return;
	}
#endif

	// count connects
	++g_tStats.m_iAgentConnect;
	g_tStats.m_iAgentRetry+= bAgentRetry;

	tAgent.m_iStartQuery = sphMicroTimer();
	tAgent.m_iWall -= tAgent.m_iStartQuery;
	if ( connect ( tAgent.m_iSock, (struct sockaddr*)&ss, len )<0 )
	{
		int iErr = sphSockGetErrno();
		if ( !(iErr==EINPROGRESS || iErr==EINTR || iErr==EWOULDBLOCK) ) // check for EWOULDBLOCK is for winsock only
		{
			tAgent.Fail ( eConnectFailures, "connect() failed: errno=%d, %s", iErr, sphSockError(iErr) );
			tAgent.State ( AGENT_RETRY ); // do retry on connect() failures
			return;
		}
		// connection in progress
		tAgent.State ( AGENT_CONNECTING );
	} else
	{
		// connect() success
		track_processing_time ( tAgent );
		// send the client's proto version right now to avoid w-w-r pattern.
		if ( SendHandshake ( tAgent ) )
			tAgent.State ( AGENT_HANDSHAKE );
	}
}

// process states AGENT_CONNECTING, AGENT_HANDSHAKE, AGENT_ESTABLISHED and notes AGENT_QUERYED
// called in serial order after RemoteConnectToAgents (so, the context is NOT changed during the call).
int RemoteQueryAgents ( AgentConnectionContext_t * pCtx )
{
	assert ( pCtx->m_iTimeout>=0 );
	assert ( pCtx->m_ppAgents );
	assert ( pCtx->m_iAgentCount );

	sphLogDebugv ( "RemoteQueryAgents for %d agents, timeout %d", pCtx->m_iAgentCount, pCtx->m_iTimeout );

	int iAgents = 0;
	int64_t tmMaxTimer = sphMicroTimer() + pCtx->m_iTimeout*1000; // in microseconds

	ISphNetEvents* pEvents = sphCreatePoll ( pCtx->m_iAgentCount, true );
	bool bTimeout = false;

	// charge poller with appropriate agents
	for ( int i = 0; i<pCtx->m_iAgentCount; i++ )
	{
		AgentConn_t * pAgent = pCtx->m_ppAgents[i];
		// select only 'initial' agents - which are not send query response.
		if ( NonQueryCond ( pAgent->State () ) )
			continue;

		assert ( !pAgent->m_tDesc.m_sPath.IsEmpty () || pAgent->m_tDesc.m_iPort>0 );
		assert ( pAgent->m_iSock>0 );
		if ( pAgent->m_iSock<=0 || ( pAgent->m_tDesc.m_sPath.IsEmpty () && pAgent->m_tDesc.m_iPort<=0 ) )
		{
			pAgent->Fail ( eConnectFailures, "invalid agent in querying. Socket %d, Path %s, Port %d", pAgent->m_iSock
						   , pAgent->m_tDesc.m_sPath.cstr (), pAgent->m_tDesc.m_iPort );
			continue;
		}
		pEvents->SetupEvent ( pAgent->m_iSock,
			( pAgent->State ()==AGENT_CONNECTING || pAgent->State ()==AGENT_ESTABLISHED )
				? ISphNetEvents::SPH_POLL_WR
				: ISphNetEvents::SPH_POLL_RD
			, pAgent );
	}

	// main loop of querying
	while (true)
	{
		bool bDone = true;
		for ( int i=0; i<pCtx->m_iAgentCount; i++ )
		{
			AgentConn_t * pAgent = pCtx->m_ppAgents[i];
			// select only 'initial' agents - which are not send query response.
			if ( NonQueryCond ( pAgent->State () ) )
				continue;
			if ( pAgent->State()!=AGENT_QUERYED )
			{
				bDone = false;
				break;
			}
		}
		if ( bDone )
			break;

		// compute timeout
		int64_t tmSelect = sphMicroTimer();
		int64_t tmMicroLeft = tmMaxTimer - tmSelect;
		if ( tmMicroLeft<=0 )
		{
			bTimeout = true;
			break; // FIXME? what about iTimeout==0 case?
		}

		// do poll
		bool bHaveSelected = pEvents->Wait( int( tmMicroLeft/1000 ) );

		// update counters, and loop again if nothing happened
		pCtx->m_ppAgents[0]->m_iWaited += sphMicroTimer() - tmSelect;
		// todo: do we need to check for EINTR here? Or the fact of timeout is enough anyway?
		if ( !bHaveSelected )
			continue;

		pEvents->IterateStart();

		// ok, something did happen, so loop the agents and do them checks
		while ( pEvents->IterateNextReady() )
		{
			NetEventsIterator_t & tEvent = pEvents->IterateGet ();
			AgentConn_t & tAgent = *(AgentConn_t*)tEvent.m_pData;
			bool bErr = ( (tEvent.m_uEvents & ( ISphNetEvents::SPH_POLL_ERR | ISphNetEvents::SPH_POLL_HUP ) )!=0 );

			if ( tAgent.State ()==AGENT_CONNECTING && bErr )
			{
				pEvents->IterateRemove ( tAgent.m_iSock );
				int iErr = 0;
				socklen_t iErrLen = sizeof ( iErr );
				if ( getsockopt ( tAgent.m_iSock, SOL_SOCKET, SO_ERROR, ( char * ) &iErr, &iErrLen )<0 )
					sphWarning ( "failed to get error: %d '%s'", errno, strerror ( errno ) );
				// connect() failure
				tAgent.Fail ( eConnectFailures, "connect() failed: errno=%d, %s", iErr, sphSockError ( iErr ) );
				continue;
			}

			// connect() success
			if ( tAgent.State()==AGENT_CONNECTING && tEvent.IsWritable ()  )
			{
				track_processing_time ( tAgent );

				// send the client's proto version right now to avoid w-w-r pattern.
				if ( SendHandshake ( tAgent, pEvents ) )
					tAgent.State ( AGENT_HANDSHAKE );
				continue;
			}

			// check if hello was received
			if ( tAgent.State()==AGENT_HANDSHAKE && tEvent.IsReadable () )
			{
				if ( CheckRemoteVer (tAgent, pEvents))
					tAgent.State ( AGENT_ESTABLISHED );
				continue;
			}

			if ( tAgent.State()==AGENT_ESTABLISHED && tEvent.IsWritable () )
			{
				if ( SendRequest ( tAgent, pCtx->m_pBuilder, pEvents ))
				{
					tAgent.State ( AGENT_QUERYED );
					++iAgents;
				}

				continue;
			}

			// check if queried agent replied while we were querying others
			if ( tAgent.State()==AGENT_QUERYED && tEvent.IsReadable () )
			{
				// do not account agent wall time from here; agent is probably ready
				tAgent.m_iWall += sphMicroTimer();
				tAgent.State ( AGENT_PREREPLY );
				pEvents->IterateRemove(tAgent.m_iSock);
				continue;
			}
		}
	}
	SafeDelete (pEvents);


	// check if connection timed out
	for ( int i=0; i<pCtx->m_iAgentCount; i++ )
	{
		AgentConn_t * pAgent = pCtx->m_ppAgents[i];
		if ( bTimeout && !( NonQueryCond ( pAgent->State () ) ))
		{
			// technically, we can end up here via two different routes
			// a) connect() never finishes in given time frame
			// b) agent actually accept()s the connection but keeps silence
			// however, there's no way to tell the two from each other
			// so we just account both cases as connect() failure
			pAgent->Fail ( eTimeoutsConnect, "connect() timed out" );
		}
	}

	return iAgents;
}

// processing states AGENT_QUERYED/AGENT_PREREPLY, and AGENT_REPLY
// may work in parallel with RemoteQueryAgents, so the state MAY change during a call.
int RemoteWaitForAgents ( AgentsVector & dAgents, int iTimeout, IReplyParser_t & tParser )
{
	assert ( iTimeout>=0 );
	sphLogDebugv ( "RemoteWaitForAgents for %d agents, timeout %d", dAgents.GetLength (), iTimeout);


	int iAgents = 0;
	int iEvents = 0;
	bool bTimeout = false;

	int64_t tmMaxTimer = sphMicroTimer() + iTimeout*1000; // in microseconds
	ISphNetEvents * pEvents = nullptr;

	while (true)
	{
		if ( !iEvents )
			for ( const auto * pAgent : dAgents )
			{
				if ( InWaitCond ( pAgent->State() ) )
				{
					assert ( !pAgent->m_tDesc.m_sPath.IsEmpty() || pAgent->m_tDesc.m_iPort>0 );
					assert ( pAgent->m_iSock>0 );
					if (!pEvents)
						pEvents = sphCreatePoll ( dAgents.GetLength (), true );
					pEvents->SetupEvent ( pAgent->m_iSock, ISphNetEvents::SPH_POLL_RD, pAgent );
					++iEvents;
					// don't break since iEvents also counted
				}
			}

		if ( !iEvents )
			break;

		int64_t tmSelect = sphMicroTimer();
		int64_t tmMicroLeft = tmMaxTimer - tmSelect;
		if ( tmMicroLeft<=0 ) // FIXME? what about iTimeout==0 case?
		{
			bTimeout = true;
			break;
		}

		bool bHaveAnswered = pEvents->Wait( int( tmMicroLeft/1000 ) );
		dAgents[0]->m_iWaited += sphMicroTimer() - tmSelect;

		if ( !bHaveAnswered )
			continue; // check for double-add above

		pEvents->IterateStart();
		while ( pEvents->IterateNextReady() )
		{
			NetEventsIterator_t & tEvent = pEvents->IterateGet();
			AgentConn_t & tAgent = *(AgentConn_t *)tEvent.m_pData;
			if ( !( InWaitCond ( tAgent.State () ) ) )
				continue;

			if ( !( tEvent.IsReadable () ) )
				continue;

			// if there was no reply yet, read reply header
			bool bWarnings = false;
			// need real socket for poller, however might be lost on agent failure
			int iSock = tAgent.m_iSock;

			if ( tAgent.State ()==AGENT_PREREPLY )
			{
				tAgent.m_iWall -= sphMicroTimer ();
				tAgent.State ( AGENT_QUERYED );
			}

			if ( tAgent.State()==AGENT_QUERYED )
			{
				if ( !CheckReplyHeader ( tAgent, pEvents ) )
					continue;
				tAgent.State ( AGENT_REPLY );
			}

			assert ( tAgent.State() == AGENT_REPLY );

			// if we are reading reply, read another chunk
			// do read
			if ( !GetReplyChunk ( tAgent, pEvents ) )
				continue; // check for double-add above! No deletion here!

			pEvents->IterateRemove ( iSock );
			--iEvents;

			// if reply was fully received, parse it
			if ( tAgent.m_iReplyRead==tAgent.m_iReplySize )
			{
				MemInputBuffer_c tReq ( tAgent.m_dReplyBuf.Begin(), tAgent.m_iReplySize );

				// absolve thy former sins
				tAgent.m_sFailure = "";

				// check for general errors/warnings first
				auto bEscapeLoop = true;
				switch ( tAgent.m_eReplyStatus )
				{
				case SEARCHD_RETRY:
					tAgent.State ( AGENT_RETRY );
					tAgent.m_sFailure.SetSprintf ( "remote warning: %s", tReq.GetString ().cstr () );
					break;

				case SEARCHD_ERROR:
					tAgent.m_sFailure.SetSprintf ( "remote error: %s", tReq.GetString ().cstr () );
					break;

				case SEARCHD_WARNING:
					tAgent.m_sFailure.SetSprintf ( "remote warning: %s", tReq.GetString ().cstr () );
					bWarnings = true;
					// no break;

				case SEARCHD_OK:
				default: bEscapeLoop = false;
				}

				if ( bEscapeLoop )
				{
					AgentFailure (tAgent);
					continue;
				}

				if ( !tParser.ParseReply ( tReq, tAgent ) )
				{
					AgentFailure (tAgent);
					continue;
				}

				// check if there was enough data
				if ( tReq.GetError() )
				{
					AgentFailure ( tAgent );
					tAgent.Fail ( eWrongReplies, "incomplete reply" );
					continue;
				}

				// all is well
				++iAgents;
				tAgent.Close ( false );
				tAgent.m_bSuccess = true;
				tAgent.m_bDone = true;
				ARRAY_FOREACH_COND ( i, tAgent.m_dResults, !bWarnings )
					bWarnings = !tAgent.m_dResults[i].m_sWarning.IsEmpty ();
				agent_stats_inc ( tAgent, bWarnings ? eNetworkCritical : eNetworkNonCritical );
			}
		}
	}

	SafeDelete ( pEvents );

	// close timed-out agents
	for ( auto * pAgent : dAgents )
	{
		if ( bTimeout && InWaitCond ( pAgent->State () ) && pAgent->m_iReplyRead!=pAgent->m_iReplySize )
		{
			assert ( !pAgent->m_dResults.GetLength() );
			assert ( !pAgent->m_bSuccess );
			pAgent->Fail ( eTimeoutsQuery, "query timed out" );
		}
	}

	return iAgents;
}

struct AgentWorkContext_t;
typedef void ( *ThdWorker_fn ) ( AgentWorkContext_t * );

struct AgentWorkContext_t : public AgentConnectionContext_t
{
	ThdWorker_fn	m_pfn = nullptr;			///< work functor & flag of dummy element
	int64_t			m_tmWait = 0;
	int				m_iAgentsDone = 0;
};


class ThdWorkPool_c : ISphNoncopyable
{
private:
	CSphMutex m_tDataLock;
	CSphMutex m_tStatLock;
public:
	CSphAutoEvent m_tChanged;
	CSphAtomic m_iActiveThreads;

private:
	CircularBuffer_T<AgentWorkContext_t> m_dWork;	// works array

	CSphAtomic m_iWorksCount;				// count of works to be done
	volatile int m_iAgentsDone;				// count of agents that finished their works
	volatile int m_iAgentsReported;			// count of agents that reported of their work done
	volatile bool m_bIsDestroying;		// help to keep at least 1 worker thread active

	CrashQuery_t m_tCrashQuery;		// query that got reported on crash

public:
	explicit ThdWorkPool_c ( int iLen );
	~ThdWorkPool_c ();


	void Pop ( AgentWorkContext_t & tNext );

	void Push ( const AgentWorkContext_t & tElem );
	void RawPush ( const AgentWorkContext_t & tElem );
	int GetReadyCount () const;
	int FetchReadyCount (); // each call returns new portion of number

	int GetReadyTotal () const
	{
		return m_iAgentsDone;
	}

	bool HasIncompleteWorks () const
	{
		return ( m_iWorksCount.GetValue()>0 );
	}

	static void PoolThreadFunc ( void * pArg );
};

ThdWorkPool_c::ThdWorkPool_c ( int iLen )
	: m_dWork ( iLen )
{
	m_tCrashQuery = SphCrashLogger_c::GetQuery(); // transfer query info for crash logger to new thread

	m_iAgentsDone = m_iAgentsReported = 0;
	m_bIsDestroying = false;

	m_tChanged.Init ( &m_tStatLock );
}

ThdWorkPool_c::~ThdWorkPool_c ()
{
	m_bIsDestroying = true;
	while ( m_iActiveThreads>0 )
		sphSleepMsec ( 1 );
	m_tChanged.Done();
}

void ThdWorkPool_c::Pop ( AgentWorkContext_t & tNext )
{
	tNext = AgentWorkContext_t();
	if ( !m_dWork.GetLength() ) // quick path for empty pool
		return;

	CSphScopedLock<CSphMutex> tData ( m_tDataLock ); // lock on create, unlock on destroy

	if ( !m_dWork.GetLength() ) // it might be empty now as another thread could steal work till that moment
		return;

	tNext = m_dWork.Pop();
	assert ( tNext.m_pfn );
}

void ThdWorkPool_c::Push ( const AgentWorkContext_t & tElem )
{
	if ( !tElem.m_pfn )
		return;

	CSphScopedLock<CSphMutex> tData ( m_tDataLock );
	m_dWork.Push() = tElem;
	m_iWorksCount.Inc();
}

void ThdWorkPool_c::RawPush ( const AgentWorkContext_t & tElem )
{
	m_dWork.Push() = tElem;
	m_iWorksCount.Inc();
}

int ThdWorkPool_c::FetchReadyCount ()
{
	// it could be better to lock here to get accurate value of m_iAgentsDone
	// however that make lock contention of 1 sec on 1000 query ~ total 3.2sec vs 2.2 sec ( trunk )
	int iNowDone = GetReadyCount();
	m_iAgentsReported += iNowDone;
	return iNowDone;
}

int ThdWorkPool_c::GetReadyCount () const
{
	// it could be better to lock here to get accurate value of m_iAgentsDone
	// however that make lock contention of 1 sec on 1000 query ~ total 3.2sec vs 2.2 sec ( trunk )
	return m_iAgentsDone - m_iAgentsReported;
}

void ThdWorkPool_c::PoolThreadFunc ( void * pArg )
{
	ThdWorkPool_c * pPool = (ThdWorkPool_c *)pArg;
	assert (pPool);
	++pPool->m_iActiveThreads;
	sphLogDebugv ("Thread func started for %p, now %d threads active", pArg, (DWORD)pPool->m_iActiveThreads);
	SphCrashLogger_c::SetLastQuery ( pPool->m_tCrashQuery );

	AgentWorkContext_t tNext;
	for ( ; !pPool->m_bIsDestroying; )
	{
		if ( !tNext.m_pfn ) // pop new work if current is done
		{
			pPool->Pop ( tNext );
			if ( !tNext.m_pfn ) // if there is no work at queue - worker done
			{
				// keep all threads alive due to retry will add works from outside
				// FIXME!!! switch to common worker pool
				if ( !pPool->m_bIsDestroying )
				{
					sphSleepMsec ( 1 );
					continue;
				} else
				{
					break;
				}
			}
		}

		tNext.m_pfn ( &tNext );
		pPool->m_iWorksCount.Dec();
		if ( tNext.m_iAgentsDone || !tNext.m_pfn )
		{
			CSphScopedLock<CSphMutex> tStat ( pPool->m_tStatLock );
			pPool->m_iAgentsDone += tNext.m_iAgentsDone;
			pPool->m_tChanged.SetEvent();
		}

		pPool->Push ( tNext );
		tNext = AgentWorkContext_t();
	}
	--pPool->m_iActiveThreads;
	sphLogDebugv ( "Thread func finished for %p, now %d threads active", pArg, (DWORD)pPool->m_iActiveThreads );
}

void ThdWorkParallel ( AgentWorkContext_t * );
void ThdWorkWait ( AgentWorkContext_t * pCtx )
{
	pCtx->m_pfn = ( pCtx->m_tmWait<sphMicroTimer() ) ? ThdWorkWait : ThdWorkParallel;
}

void SetNextRetry ( AgentWorkContext_t * pCtx )
{
	pCtx->m_pfn = NULL;
	pCtx->m_ppAgents[0]->State ( AGENT_UNUSED );

	// reconnect only on state == UNUSED and agent not just finished sequence (m_bDone)
	if ( pCtx->m_ppAgents[0]->m_bDone || !pCtx->m_ppAgents[0]->m_iRetryLimit || !pCtx->m_iDelay || pCtx->m_ppAgents[0]->m_iRetries>pCtx->m_ppAgents[0]->m_iRetryLimit )
		return;

	int64_t tmNextTry = sphMicroTimer() + pCtx->m_iDelay*1000;
	pCtx->m_pfn = ThdWorkWait;
	++pCtx->m_ppAgents[0]->m_iRetries;
	pCtx->m_tmWait = tmNextTry;
	pCtx->m_ppAgents[0]->State ( AGENT_RETRY );
	pCtx->m_ppAgents[0]->NextMirror();
}

void ThdWorkParallel ( AgentWorkContext_t * pCtx )
{
	sphLogDebugv ( "parallel worker, sock=%d", pCtx->m_ppAgents[0]->m_iSock );

	RemoteConnectToAgent ( *pCtx->m_ppAgents[0] );
	if ( pCtx->m_ppAgents[0]->State()==AGENT_UNUSED )
	{
		SetNextRetry ( pCtx );
		return;
	}

	RemoteQueryAgents ( pCtx );
	if ( pCtx->m_ppAgents[0]->State()==AGENT_RETRY ) // next round of connect try
	{
		SetNextRetry ( pCtx );
	} else
	{
		pCtx->m_pfn = NULL;
		pCtx->m_iAgentsDone = 1;
	}
}


void ThdWorkSequental ( AgentWorkContext_t * pCtx )
{
	sphLogDebugv ( "seq worker" );

	if ( pCtx->m_ppAgents[0]->m_iRetries )
		sphSleepMsec ( pCtx->m_iDelay );

	for ( int iAgent=0; iAgent<pCtx->m_iAgentCount; iAgent++ )
	{
		AgentConn_t * pAgent = pCtx->m_ppAgents[iAgent];
		// connect or reconnect only
		// + initially - iRetries == 0 and state == UNUSED and agent not just finished sequence (m_bDone)
		// + retry - state == RETRY
		if ( ( pAgent->m_iRetries==0 && pAgent->State()==AGENT_UNUSED && !pAgent->m_bDone ) || pAgent->State()==AGENT_RETRY )
			RemoteConnectToAgent ( *pAgent );
	}

	pCtx->m_iAgentsDone += RemoteQueryAgents ( pCtx );

	bool bNeedRetry = false;
	if ( pCtx->m_ppAgents[0]->m_iRetryLimit )
	{
		for ( int i=0; i<pCtx->m_iAgentCount; i++ )
			if ( pCtx->m_ppAgents[i]->State()==AGENT_RETRY )
			{
				bNeedRetry = true;
				break;
			}
	}

	pCtx->m_pfn = NULL;
	if ( bNeedRetry )
	{
		for ( int i = 0; i<pCtx->m_iAgentCount; i++ )
		{
			AgentConn_t * pAgent = pCtx->m_ppAgents[i];
			if ( pAgent->State()==AGENT_RETRY )
			{
				++pAgent->m_iRetries;
				if ( pAgent->m_iRetries<pAgent->m_iRetryLimit )
				{
					pCtx->m_pfn = ThdWorkSequental;
					pAgent->NextMirror();
				} else
					pAgent->State( AGENT_UNUSED );
			}
		}
	}
}

class CSphRemoteAgentsController : public ISphRemoteAgentsController
{
public:
	CSphRemoteAgentsController ( int iThreads, AgentsVector & dAgents, const IRequestBuilder_t & tBuilder, int iTimeout, int iRetryMax=0, int iDelay=0 );

	virtual ~CSphRemoteAgentsController ();


	// check that there are no works to do
	virtual bool IsDone () const override
	{
		return !m_pWorkerPool->HasIncompleteWorks();
	}

	// block execution while there are works to do
	virtual int Finish () override;

	// check that some agents are done at this iteration
	virtual bool HasReadyAgents () const override
	{
		return (m_pWorkerPool->GetReadyCount()>0 );
	}

	virtual int FetchReadyAgents () override
	{
		return m_pWorkerPool->FetchReadyCount();
	}

	virtual void WaitAgentsEvent () override;

	virtual int RetryFailed () override;

private:
	ThdWorkPool_c * m_pWorkerPool;
	CSphVector<SphThread_t> m_dThds;

	// stores params from ctr to work of RetryFailed
	AgentsVector * m_pAgents;
	const IRequestBuilder_t * m_pBuilder;
	int m_iTimeout;
	int m_iDelay;

};

CSphRemoteAgentsController::CSphRemoteAgentsController ( int iThreads, AgentsVector & dAgents, const IRequestBuilder_t & tBuilder, int iTimeout, int iRetryMax, int iDelay )
	: m_pWorkerPool ( new ThdWorkPool_c ( dAgents.GetLength() ) )
{
	assert ( dAgents.GetLength() );

	m_pAgents = &dAgents;
	m_pBuilder = &tBuilder;
	m_iTimeout = iTimeout;
	m_iDelay = iDelay;

	iThreads = Max ( 1, Min ( iThreads, dAgents.GetLength() ) );
	m_dThds.Resize ( iThreads );

	AgentWorkContext_t tCtx;
	tCtx.m_pBuilder = &tBuilder;
	tCtx.m_iAgentCount = 1;
	tCtx.m_pfn = ThdWorkParallel;
	tCtx.m_iDelay = iDelay;
	tCtx.m_iTimeout = iTimeout;

	if ( iThreads>1 )
	{
		ARRAY_FOREACH ( i, dAgents )
		{
			tCtx.m_ppAgents = dAgents.Begin()+i;
			tCtx.m_ppAgents[0]->m_iRetryLimit = iRetryMax * tCtx.m_ppAgents[0]->GetMirrorsCount();
			m_pWorkerPool->RawPush ( tCtx );
		}
	} else
	{
		tCtx.m_ppAgents = dAgents.Begin();
		tCtx.m_iAgentCount = dAgents.GetLength();
		for ( AgentConn_t * pAgent : dAgents )
			pAgent->m_iRetryLimit = iRetryMax * pAgent->GetMirrorsCount();
		tCtx.m_pfn = ThdWorkSequental;
		m_pWorkerPool->RawPush ( tCtx );
	}

	ARRAY_FOREACH ( i, m_dThds )
		SphCrashLogger_c::ThreadCreate ( m_dThds.Begin()+i, ThdWorkPool_c::PoolThreadFunc, m_pWorkerPool );
}

CSphRemoteAgentsController::~CSphRemoteAgentsController ()
{
	SafeDelete (m_pWorkerPool);
	ARRAY_FOREACH ( i, m_dThds )
		sphThreadJoin ( m_dThds.Begin()+i );

	m_dThds.Resize ( 0 );
}

// block execution while there are works to do
int CSphRemoteAgentsController::Finish ()
{
	while ( !IsDone() )
		WaitAgentsEvent();

	return m_pWorkerPool->GetReadyTotal();
}

void CSphRemoteAgentsController::WaitAgentsEvent ()
{
	m_pWorkerPool->m_tChanged.WaitEvent();
}

int CSphRemoteAgentsController::RetryFailed ()
{
	AgentWorkContext_t tCtx;
	tCtx.m_pBuilder = m_pBuilder;
	tCtx.m_iDelay = m_iDelay;
	tCtx.m_iTimeout = m_iTimeout;

	tCtx.m_ppAgents = m_pAgents->Begin();
	tCtx.m_iAgentCount = m_pAgents->GetLength();
	int iRetries=0;
	for ( AgentConn_t * pAgent : *m_pAgents )
	{
		if ( pAgent->State()==AGENT_RETRY )
		{
			++pAgent->m_iRetries;
			if ( pAgent->m_iRetries<pAgent->m_iRetryLimit )
			{
				pAgent->m_dResults.Reset ();
				pAgent->NextMirror();
				++iRetries;
			} else
				pAgent->State ( AGENT_UNUSED );
		}
	}
	if ( !iRetries )
		return 0;
	sphLogDebugv ( "Found %d agents in state AGENT_RETRY, reschedule them", iRetries );
	tCtx.m_pfn = ThdWorkSequental;
	m_pWorkerPool->Push ( tCtx );
	return iRetries;
}

ISphRemoteAgentsController* GetAgentsController ( int iThreads, AgentsVector & dAgents, const IRequestBuilder_t & tBuilder, int iTimeout, int iRetryMax, int iDelay )
{
	return new CSphRemoteAgentsController ( iThreads, dAgents, tBuilder, iTimeout, iRetryMax, iDelay );
}


/// check if a non-blocked socket is still connected
bool sphNBSockEof ( int iSock )
{
	if ( iSock<0 )
		return true;

	char cBuf;
	// since socket is non-blocked, ::recv will not block anyway
	int iRes = ::recv ( iSock, &cBuf, sizeof ( cBuf ), MSG_PEEK );
	if ( (!iRes) || (iRes<0 && sphSockGetErrno ()!=EWOULDBLOCK ))
		return true;
	return false;
}

ISphNetEvents::~ISphNetEvents () = default;

// in case of epoll/kqueue the full set of polled sockets are stored
// in a cache inside kernel, so once added, we can't iterate over all of the items.
// So, we store them in linked list for that purpose.

// wrap raw void* into ListNode_t to store it in List_t
struct ListedData_t : public ListNode_t
{
	const void * m_pData;
	explicit ListedData_t ( const void * pData ) : m_pData ( pData )
	{}
};


// store and iterate over the list of items
class IterableEvents_c : public ISphNetEvents
{
protected:
	List_t m_tWork;
	NetEventsIterator_t m_tIter;
	ListedData_t * m_pIter = nullptr;

protected:
	ListedData_t * AddNewEventData ( const void * pData )
	{
		assert ( pData );
		ListedData_t * pIntData = new ListedData_t ( pData );
		m_tWork.Add ( pIntData );
		return pIntData;
	}

	void ResetIterator ()
	{
		m_tIter.Reset();
		m_pIter = nullptr;
	}

	void RemoveCurrentItem ()
	{
		assert ( m_pIter );
		assert ( m_pIter->m_pData==m_tIter.m_pData );
		assert ( m_tIter.m_pData );

		ListedData_t * pPrev = (ListedData_t *)m_pIter->m_pPrev;
		m_tWork.Remove ( m_pIter );
		SafeDelete( m_pIter );
		m_pIter = pPrev;
		m_tIter.m_pData = m_pIter->m_pData;
	}

public:
	IterableEvents_c () = default;

	virtual ~IterableEvents_c ()
	{
		while ( m_tWork.GetLength() )
		{
			ListedData_t * pIter = (ListedData_t *)m_tWork.Begin();
			m_tWork.Remove ( pIter );
			SafeDelete( pIter );
		}
		ResetIterator();
	}

	bool IterateNextAll () override
	{
		if ( !m_pIter )
		{
			if ( m_tWork.Begin()==m_tWork.End() )
				return false;

			m_pIter = (ListedData_t *)m_tWork.Begin();
			m_tIter.m_pData = m_pIter->m_pData;
			return true;
		} else
		{
			m_pIter = (ListedData_t *)m_pIter->m_pNext;
			m_tIter.m_pData = m_pIter->m_pData;
			if ( m_pIter!=m_tWork.End() )
				return true;

			ResetIterator();
			return false;
		}
	}

	NetEventsIterator_t & IterateGet() override
	{
		return m_tIter;
	}
};


#if POLLING_EPOLL
class EpollEvents_c : public IterableEvents_c
{
private:
	CSphVector<epoll_event>		m_dReady;
	int							m_iLastReportedErrno;
	int							m_iReady;
	int							m_iEFD;
	int							m_iIterEv;

public:
	explicit EpollEvents_c ( int iSizeHint )
		: m_iLastReportedErrno ( -1 )
		, m_iReady ( 0 )
	{
		m_iEFD = epoll_create ( iSizeHint ); // 1000 is dummy, see man
		if ( m_iEFD==-1 )
			sphDie ( "failed to create epoll main FD, errno=%d, %s", errno, strerror ( errno ) );

		sphLogDebugv ( "epoll %d created", m_iEFD );
		m_dReady.Reserve ( iSizeHint );
		m_iIterEv = -1;
	}

	~EpollEvents_c ()
	{
		sphLogDebugv ( "epoll %d closed", m_iEFD );
		SafeClose ( m_iEFD );
	}

	void SetupEvent ( int iSocket, PoolEvents_e eFlags, const void * pData ) override
	{
		assert ( pData && iSocket>=0 );
		assert ( eFlags==SPH_POLL_WR || eFlags==SPH_POLL_RD );

		ListedData_t * pIntData = AddNewEventData ( pData );

		epoll_event tEv;
		tEv.data.ptr = pIntData;
		tEv.events = (eFlags==SPH_POLL_RD ? EPOLLIN : EPOLLOUT);

		sphLogDebugv ( "%p epoll %d setup, ev=0x%u, sock=%d", pData, m_iEFD, tEv.events, iSocket );

		int iRes = epoll_ctl ( m_iEFD, EPOLL_CTL_ADD, iSocket, &tEv );
		if ( iRes==-1 )
			sphWarning ( "failed to setup epoll event for sock %d, errno=%d, %s", iSocket, errno, strerror ( errno ) );
	}

	bool Wait ( int timeoutMs ) override
	{
		m_dReady.Resize ( m_tWork.GetLength () );
		// need positive timeout for communicate threads back and shutdown
		m_iReady = epoll_wait ( m_iEFD, m_dReady.Begin (), m_dReady.GetLength (), timeoutMs );

		sphLogDebugv ( "%d epoll wait returned %d events (timeout %d)", m_iEFD, m_iReady, timeoutMs );

		if ( m_iReady<0 )
		{
			int iErrno = sphSockGetErrno ();
			// common recoverable errors
			if ( iErrno==EINTR || iErrno==EAGAIN || iErrno==EWOULDBLOCK )
				return false;

			if ( m_iLastReportedErrno!=iErrno )
			{
				sphWarning ( "poll tick failed: %s", sphSockError ( iErrno ) );
				m_iLastReportedErrno = iErrno;
			}
			return false;
		}

		return ( m_iReady>0 );
	}

	int IterateStart () override
	{
		ResetIterator();
		m_iIterEv = -1;
		return m_iReady;
	}

	bool IterateNextReady () override
	{
		ResetIterator();
		++m_iIterEv;
		if ( m_iReady<=0 || m_iIterEv>=m_iReady )
			return false;

		const epoll_event & tEv = m_dReady[m_iIterEv];

		m_pIter = (ListedData_t *)tEv.data.ptr;
		m_tIter.m_pData = m_pIter->m_pData;

		if ( tEv.events & EPOLLIN )
			m_tIter.m_uEvents |= SPH_POLL_RD;
		if ( tEv.events & EPOLLOUT )
			m_tIter.m_uEvents |= SPH_POLL_WR;
		if ( tEv.events & EPOLLHUP )
			m_tIter.m_uEvents |= SPH_POLL_HUP;
		if ( tEv.events & EPOLLERR )
			m_tIter.m_uEvents |= SPH_POLL_ERR;
		if ( tEv.events & EPOLLPRI )
			m_tIter.m_uEvents |= SPH_POLL_PRI;

		return true;
	}

	void IterateChangeEvent ( int iSocket, PoolEvents_e eFlags ) override
	{
		epoll_event tEv;
		tEv.data.ptr = (void *)m_pIter;
		tEv.events = (eFlags==SPH_POLL_RD ? EPOLLIN : EPOLLOUT); ;

		sphLogDebugv ( "%p epoll change, ev=0x%u, sock=%d", m_tIter.m_pData, tEv.events, iSocket );

		int iRes = epoll_ctl ( m_iEFD, EPOLL_CTL_MOD, iSocket, &tEv );
		if ( iRes==-1 )
			sphWarning ( "failed to modify epoll event for sock %d, errno=%d, %s", iSocket, errno, strerror ( errno ) );
	}

	void IterateRemove ( int iSocket ) override
	{
		assert ( m_pIter->m_pData==m_tIter.m_pData );

		sphLogDebugv ( "%p epoll remove, ev=0x%u, sock=%d", m_tIter.m_pData, m_tIter.m_uEvents, iSocket );
		assert ( m_tIter.m_pData );

		epoll_event tEv;
		int iRes = epoll_ctl ( m_iEFD, EPOLL_CTL_DEL, iSocket, &tEv );

		// might be already closed by worker from thread pool
		if ( iRes==-1 )
			sphLogDebugv ( "failed to remove epoll event for sock %d(%p), errno=%d, %s", iSocket, m_tIter.m_pData, errno, strerror ( errno ) );

		RemoveCurrentItem();
	}
};

ISphNetEvents * sphCreatePoll ( int iSizeHint, bool )
{
	return new EpollEvents_c ( iSizeHint );
}

#endif
#if POLLING_KQUEUE

class KqueueEvents_c : public IterableEvents_c
{

private:
	CSphVector<struct kevent>			m_dReady;
	int							m_iLastReportedErrno;
	int							m_iReady;
	int							m_iKQ;
	int							m_iIterEv;

public:
	explicit KqueueEvents_c ( int iSizeHint )
		: m_iLastReportedErrno ( -1 )
		, m_iReady ( 0 )
	{
		m_iKQ = kqueue ();
		if ( m_iKQ==-1 )
			sphDie ( "failed to create kqueue main FD, errno=%d, %s", errno, strerror ( errno ) );

		sphLogDebugv ( "kqueue %d created", m_iKQ );
		m_dReady.Reserve ( iSizeHint );
		m_iIterEv = -1;
	}

	~KqueueEvents_c ()
	{
		sphLogDebugv ( "kqueue %d closed", m_iKQ );
		SafeClose ( m_iKQ );
	}

	void SetupEvent ( int iSocket, PoolEvents_e eFlags, const void * pData ) override
	{
		assert ( pData && iSocket>=0 );
		assert ( eFlags==SPH_POLL_WR || eFlags==SPH_POLL_RD );

		ListedData_t * pIntData = AddNewEventData ( pData );

		struct kevent tEv;
		EV_SET ( &tEv, iSocket, (eFlags==SPH_POLL_RD ? EVFILT_READ : EVFILT_WRITE), EV_ADD, 0, 0, pIntData );

		sphLogDebugv ( "%p kqueue %d setup, ev=%d, sock=%d", pData, m_iKQ, tEv.filter, iSocket );

		int iRes = kevent (m_iKQ, &tEv, 1, nullptr, 0, nullptr);
		if ( iRes==-1 )
			sphWarning ( "failed to setup kqueue event for sock %d, errno=%d, %s", iSocket, errno, strerror ( errno ) );
	}

	bool Wait ( int timeoutMs ) override
	{
		m_dReady.Resize ( m_tWork.GetLength () );

		timespec ts;
		timespec *pts = nullptr;
		if ( timeoutMs )
		{
			ts.tv_sec = timeoutMs/1000;
			ts.tv_nsec = (long)(timeoutMs-ts.tv_sec*1000)*1000000;
			pts = &ts;
		}
		// need positive timeout for communicate threads back and shutdown
		m_iReady = kevent (m_iKQ, nullptr, 0, m_dReady.begin(), m_dReady.GetLength(), pts);

		sphLogDebugv ( "%d kqueue wait returned %d events (timeout %d)", m_iKQ, m_iReady, timeoutMs );

		if ( m_iReady<0 )
		{
			int iErrno = sphSockGetErrno ();
			// common recoverable errors
			if ( iErrno==EINTR || iErrno==EAGAIN || iErrno==EWOULDBLOCK )
				return false;

			if ( m_iLastReportedErrno!=iErrno )
			{
				sphWarning ( "kqueue tick failed: %s", sphSockError ( iErrno ) );
				m_iLastReportedErrno = iErrno;
			}
			return false;
		}

		return ( m_iReady>0 );
	}

	int IterateStart () override
	{
		ResetIterator();
		m_iIterEv = -1;
		return m_iReady;
	}

	bool IterateNextReady () override
	{
		ResetIterator();
		++m_iIterEv;
		if ( m_iReady<=0 || m_iIterEv>=m_iReady )
			return false;

		const struct kevent & tEv = m_dReady[m_iIterEv];

		m_pIter = (ListedData_t *) tEv.udata;
		m_tIter.m_pData = m_pIter->m_pData;

		if ( tEv.filter == EVFILT_READ )
			m_tIter.m_uEvents = SPH_POLL_RD;

		if ( tEv.filter == EVFILT_WRITE )
			m_tIter.m_uEvents = SPH_POLL_WR;

		sphLogDebugv ( "%p kqueue iterate ready, ev=%d", m_tIter.m_pData, tEv.filter );

		return true;
	}

	void IterateChangeEvent ( int iSocket, PoolEvents_e eFlags ) override
	{
		assert ( eFlags==SPH_POLL_WR || eFlags==SPH_POLL_RD );



		struct kevent tEv;
		EV_SET(&tEv, iSocket, (eFlags==SPH_POLL_RD ? EVFILT_READ : EVFILT_WRITE), EV_ADD, 0, 0, (void*) m_pIter);

		sphLogDebugv ( "%p kqueue change, ev=%d, sock=%d", m_tIter.m_pData, tEv.filter, iSocket );


		int iRes = kevent (m_iKQ, &tEv, 1, nullptr, 0, nullptr);

		EV_SET( &tEv, iSocket, ( eFlags==SPH_POLL_RD ? EVFILT_WRITE : EVFILT_READ ), EV_DELETE | EV_CLEAR, 0, 0, ( void * ) m_pIter );
		kevent ( m_iKQ, &tEv, 1, nullptr, 0, nullptr );

		if ( iRes==-1 )
			sphWarning ( "failed to setup kqueue event for sock %d, errno=%d, %s", iSocket, errno, strerror ( errno ) );
	}

	void IterateRemove ( int iSocket ) override
	{
		assert ( m_pIter->m_pData==m_tIter.m_pData );

		sphLogDebugv ( "%p kqueue remove, uEv=0x%u, sock=%d", m_tIter.m_pData, m_tIter.m_uEvents, iSocket );
		assert ( m_tIter.m_pData );

		struct kevent tEv;
		EV_SET(&tEv, iSocket, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
		kevent (m_iKQ, &tEv, 1, nullptr, 0, nullptr);
		EV_SET( &tEv, iSocket, EVFILT_READ, EV_DELETE, 0, 0, nullptr );
		int iRes = kevent ( m_iKQ, &tEv, 1, nullptr, 0, nullptr );

		// might be already closed by worker from thread pool
		if ( iRes==-1 )
			sphLogDebugv ( "failed to remove kqueue event for sock %d(%p), errno=%d, %s", iSocket, m_tIter.m_pData, errno, strerror ( errno ) );

		RemoveCurrentItem ();
	}
};

ISphNetEvents * sphCreatePoll ( int iSizeHint, bool )
{
	return new KqueueEvents_c ( iSizeHint );
}

#endif
#if POLLING_POLL
class PollEvents_c : public ISphNetEvents
{
private:
	CSphVector<const void *>	m_dWork;
	CSphVector<pollfd>	m_dEvents;
	int					m_iLastReportedErrno;
	int					m_iReady;
	NetEventsIterator_t	m_tIter;
	int					m_iIter;

public:
	explicit PollEvents_c ( int iSizeHint )
		: m_iLastReportedErrno ( -1 )
		, m_iReady ( 0 )
		, m_iIter ( -1)
	{
		m_dWork.Reserve ( iSizeHint );
		m_tIter.Reset();
	}

	~PollEvents_c () = default;

	void SetupEvent ( int iSocket, PoolEvents_e eFlags, const void * pData ) override
	{
		assert ( pData && iSocket>=0 );
		assert ( eFlags==SPH_POLL_WR || eFlags==SPH_POLL_RD );

		pollfd tEvent;
		tEvent.fd = iSocket;
		tEvent.events = (eFlags==SPH_POLL_RD ? POLLIN : POLLOUT);

		assert ( m_dEvents.GetLength() == m_dWork.GetLength() );
		m_dEvents.Add ( tEvent );
		m_dWork.Add ( pData );
	}

	bool Wait ( int timeoutMs ) override
	{
		// need positive timeout for communicate threads back and shutdown
		m_iReady = ::poll ( m_dEvents.Begin (), m_dEvents.GetLength (), timeoutMs );

		if ( m_iReady<0 )
		{
			int iErrno = sphSockGetErrno ();
			// common recoverable errors
			if ( iErrno==EINTR || iErrno==EAGAIN || iErrno==EWOULDBLOCK )
				return false;

			if ( m_iLastReportedErrno!=iErrno )
			{
				sphWarning ( "poll tick failed: %s", sphSockError ( iErrno ) );
				m_iLastReportedErrno = iErrno;
			}
			return false;
		}

		return ( m_iReady>0 );
	}

	bool IterateNextAll () override
	{
		assert ( m_dEvents.GetLength ()==m_dWork.GetLength () );

		++m_iIter;
		m_tIter.m_pData = ( m_iIter<m_dWork.GetLength () ? m_dWork[m_iIter] : nullptr );

		return ( m_iIter<m_dWork.GetLength () );
	}

	bool IterateNextReady () override
	{
		m_tIter.Reset();

		if ( m_iReady<=0 || m_iIter>=m_dEvents.GetLength () )
			return false;

		while (true)
		{
			++m_iIter;
			if ( m_iIter>=m_dEvents.GetLength () )
				return false;

			if ( m_dEvents[m_iIter].revents==0 )
				continue;

			--m_iReady;

			m_tIter.m_pData = m_dWork[m_iIter];
			pollfd & tEv = m_dEvents[m_iIter];
			if ( tEv.revents & POLLIN )
				m_tIter.m_uEvents |= SPH_POLL_RD;

			if ( tEv.revents & POLLOUT )
				m_tIter.m_uEvents |= SPH_POLL_WR;

			if ( tEv.revents & POLLHUP )
				m_tIter.m_uEvents |= SPH_POLL_HUP;

			if ( tEv.revents & POLLERR )
				m_tIter.m_uEvents |= SPH_POLL_ERR;

			tEv.revents = 0;
			return true;
		}
	}

	void IterateChangeEvent ( int iSocket, PoolEvents_e eFlags ) override
	{
		assert ( m_iIter>=0 && m_iIter<m_dEvents.GetLength () );
		assert ( (SOCKET)iSocket == m_dEvents[m_iIter].fd );
		m_dEvents[m_iIter].events = (eFlags==SPH_POLL_RD ? POLLIN : POLLOUT);
	}

	void IterateRemove ( int iSocket ) override
	{
		assert ( m_iIter>=0 && m_iIter<m_dEvents.GetLength () );
		assert ( m_dEvents.GetLength ()==m_dWork.GetLength () );
		assert ( (SOCKET)iSocket == m_dEvents[m_iIter].fd );

		m_dEvents.RemoveFast ( m_iIter );
		// SafeDelete ( m_dWork[m_iIter] );
		m_dWork.RemoveFast ( m_iIter );

		--m_iIter;
		m_tIter.m_pData = nullptr;
	}

	int IterateStart () override
	{
		m_iIter = -1;
		m_tIter.Reset();

		return m_iReady;
	}

	NetEventsIterator_t & IterateGet () override
	{
		assert ( m_iIter>=0 && m_iIter<m_dWork.GetLength () );
		return m_tIter;
	}
};

ISphNetEvents * sphCreatePoll ( int iSizeHint, bool )
{
	return new PollEvents_c ( iSizeHint );
}

#endif
#if POLLING_SELECT

// used as fallback if no of modern (at least poll) functions available
class SelectEvents_c : public ISphNetEvents
{
private:
	CSphVector<const void *> m_dWork;
	CSphVector<int> m_dSockets;
	fd_set			m_fdsRead;
	fd_set			m_fdsReadResult;
	fd_set			m_fdsWrite;
	fd_set 			m_fdsWriteResult;
	int	m_iMaxSocket;
	int m_iLastReportedErrno;
	int m_iReady;
	NetEventsIterator_t m_tIter;
	int m_iIter;

public:
	explicit SelectEvents_c ( int iSizeHint )
		: m_iMaxSocket ( 0 ),
		m_iLastReportedErrno ( -1 ),
		m_iReady ( 0 ),
		m_iIter ( -1 )
	{
		m_dWork.Reserve ( iSizeHint );

		FD_ZERO ( &m_fdsRead );
		FD_ZERO ( &m_fdsWrite );

		m_tIter.Reset();
	}

	~SelectEvents_c () = default;

	void SetupEvent ( int iSocket, PoolEvents_e eFlags, const void * pData ) override
	{
		assert ( pData && iSocket>=0 );
		assert ( eFlags==SPH_POLL_WR || eFlags==SPH_POLL_RD );

		sphFDSet ( iSocket, (eFlags==SPH_POLL_RD ? &m_fdsRead : &m_fdsWrite));
		m_iMaxSocket = Max ( m_iMaxSocket, iSocket );

		assert ( m_dSockets.GetLength ()==m_dWork.GetLength () );
		m_dWork.Add ( pData );
		m_dSockets.Add (iSocket);
	}

	bool Wait ( int timeoutMs ) override
	{
		struct timeval tvTimeout;

		tvTimeout.tv_sec = (int) (timeoutMs / 1000); // full seconds
		tvTimeout.tv_usec = (int) (timeoutMs % 1000) * 1000; // microseconds
		m_fdsReadResult = m_fdsRead;
		m_fdsWriteResult = m_fdsWrite;
		m_iReady = ::select ( 1 + m_iMaxSocket, &m_fdsReadResult, &m_fdsWriteResult, NULL, &tvTimeout );

		if ( m_iReady<0 )
		{
			int iErrno = sphSockGetErrno ();
			// common recoverable errors
			if ( iErrno==EINTR || iErrno==EAGAIN || iErrno==EWOULDBLOCK )
				return false;

			if ( m_iLastReportedErrno!=iErrno )
			{
				sphWarning ( "poll (select version) tick failed: %s", sphSockError ( iErrno ) );
				m_iLastReportedErrno = iErrno;
			}
			return false;
		}

		return (m_iReady>0);
	}

	bool IterateNextAll () override
	{
		assert ( m_dSockets.GetLength ()==m_dWork.GetLength () );

		++m_iIter;
		m_tIter.m_pData = (m_iIter<m_dWork.GetLength () ? m_dWork[m_iIter] : nullptr);

		return (m_iIter<m_dWork.GetLength ());
	}

	bool IterateNextReady () override
	{
		m_tIter.Reset ();
		if ( m_iReady<=0 || m_iIter>=m_dWork.GetLength () )
			return false;

		while (true)
		{
			++m_iIter;
			if ( m_iIter>=m_dWork.GetLength () )
				return false;

			bool bReadable = FD_ISSET ( m_dSockets[m_iIter], &m_fdsReadResult );
			bool bWritable = FD_ISSET ( m_dSockets[m_iIter], &m_fdsWriteResult );

			if ( !(bReadable || bWritable))
				continue;

			--m_iReady;

			m_tIter.m_pData = m_dWork[m_iIter];

			if ( bReadable )
				m_tIter.m_uEvents |= SPH_POLL_RD;
			if ( bWritable )
				m_tIter.m_uEvents |= SPH_POLL_WR;

			return true;
		}
	}

	void IterateChangeEvent ( int iSocket, PoolEvents_e eFlags ) override
	{
		assert ( m_iIter>=0 && m_iIter<m_dSockets.GetLength () );
		int iSock = m_dSockets[m_iIter];
		assert ( iSock == iSocket );
		fd_set * pseton = (eFlags==SPH_POLL_RD ? &m_fdsRead : &m_fdsWrite);
		fd_set * psetoff = (eFlags==SPH_POLL_RD ? &m_fdsWrite : &m_fdsRead);
		if ( FD_ISSET ( iSock, psetoff) ) sphFDClr ( iSock, psetoff );
		if ( !FD_ISSET ( iSock, pseton ) ) sphFDSet ( iSock, pseton );
	}

	void IterateRemove ( int iSocket ) override
	{
		assert ( m_iIter>=0 && m_iIter<m_dSockets.GetLength () );
		assert ( m_dSockets.GetLength ()==m_dWork.GetLength () );
		assert ( iSocket==m_dSockets[m_iIter] );

		int iSock = m_dSockets[m_iIter];
		if ( FD_ISSET ( iSock, &m_fdsWrite ) ) sphFDClr ( iSock, &m_fdsWrite );
		if ( FD_ISSET ( iSock, &m_fdsRead ) ) sphFDClr ( iSock, &m_fdsRead );
		m_dSockets.RemoveFast ( m_iIter );
		// SafeDelete ( m_dWork[m_iIter] );
		m_dWork.RemoveFast ( m_iIter );

		--m_iIter;
		m_tIter.Reset();
	}

	int IterateStart () override
	{
		m_iIter = -1;
		m_tIter.Reset();

		return m_iReady;
	}

	NetEventsIterator_t &IterateGet () override
	{
		assert ( m_iIter>=0 && m_iIter<m_dWork.GetLength () );
		return m_tIter;
	}
};

class DummyEvents_c : public ISphNetEvents
{
	NetEventsIterator_t m_tIter;

public:
	void SetupEvent ( int, PoolEvents_e, const void * ) override {}
	bool Wait ( int ) override { return false; } // NOLINT
	bool IterateNextAll () override { return false; }
	bool IterateNextReady () override { return false; }
	void IterateChangeEvent ( int, PoolEvents_e ) override {}
	void IterateRemove ( int ) override {}
	int IterateStart () override { return 0; }
	NetEventsIterator_t & IterateGet () override { return m_tIter; }
};

ISphNetEvents * sphCreatePoll (int iSizeHint, bool bFallbackSelect)
{
	if (!bFallbackSelect)
		return new DummyEvents_c;

	return new SelectEvents_c( iSizeHint);

}

#endif