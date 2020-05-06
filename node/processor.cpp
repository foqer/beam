// Copyright 2018 The Beam Team
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "processor.h"
#include "../core/treasury.h"
#include "../core/shielded.h"
#include "../core/serialization_adapters.h"
#include "../utility/serialize.h"
#include "../utility/logger.h"
#include "../utility/logger_checkpoints.h"
#include <condition_variable>
#include <cctype>

namespace beam {

void NodeProcessor::OnCorrupted()
{
	CorruptionException exc;
	exc.m_sErr = "node data";
	throw exc;
}

NodeProcessor::Horizon::Horizon()
{
	SetInfinite();
}

void NodeProcessor::Horizon::SetInfinite()
{
	m_Branching = MaxHeight;
	m_Sync.Lo = MaxHeight;
	m_Sync.Hi = MaxHeight;
	m_Local.Lo = MaxHeight;
	m_Local.Hi = MaxHeight;
}

void NodeProcessor::Horizon::SetStdFastSync()
{
	uint32_t r = Rules::get().MaxRollback;
	m_Branching = r / 4; // inferior branches would be pruned when height difference is this.

	m_Sync.Hi = r;
	m_Sync.Lo = r * 3; // 3-day period

	m_Local.Hi = r * 2; // slightly higher than m_Sync.Loc, to feed other fast synchers
	m_Local.Lo = r * 180; // 180-day period
}

void NodeProcessor::Horizon::Normalize()
{
	std::setmax(m_Branching, Height(1));

	Height r = Rules::get().MaxRollback;

	std::setmax(m_Sync.Hi, std::max(r, m_Branching));
	std::setmax(m_Sync.Lo, m_Sync.Hi);

	// Some nodes in production have a bug: if (Sync.Lo == Sync.Hi) - the last generated block that they send may be incorrect
	// Workaround: make sure (Sync.Lo > Sync.Hi), at least by 1
	//
	// After HF2 the workaround can be removed
	if ((m_Sync.Lo == m_Sync.Hi) && (m_Sync.Hi < MaxHeight))
		m_Sync.Lo++;

	// though not required, we prefer m_Local to be no less than m_Sync
	std::setmax(m_Local.Hi, m_Sync.Hi);
	std::setmax(m_Local.Lo, std::max(m_Local.Hi, m_Sync.Lo));
}

void NodeProcessor::Initialize(const char* szPath)
{
	StartParams sp; // defaults
	Initialize(szPath, sp);
}

void NodeProcessor::Initialize(const char* szPath, const StartParams& sp)
{
	m_DB.Open(szPath);
	m_DbTx.Start(m_DB);

	if (sp.m_CheckIntegrity)
	{
		LOG_INFO() << "DB integrity check...";
		m_DB.CheckIntegrity();
	}

	Merkle::Hash hv;
	Blob blob(hv);

	ZeroObject(m_Extra);
	m_Extra.m_Fossil = m_DB.ParamIntGetDef(NodeDB::ParamID::FossilHeight, Rules::HeightGenesis - 1);
	m_Extra.m_TxoLo = m_DB.ParamIntGetDef(NodeDB::ParamID::HeightTxoLo, Rules::HeightGenesis - 1);
	m_Extra.m_TxoHi = m_DB.ParamIntGetDef(NodeDB::ParamID::HeightTxoHi, Rules::HeightGenesis - 1);

	m_Extra.m_ShieldedOutputs = m_DB.ParamIntGetDef(NodeDB::ParamID::ShieldedOutputs);
	m_Mmr.m_Shielded.m_Count = m_DB.ParamIntGetDef(NodeDB::ParamID::ShieldedInputs);
	m_Mmr.m_Shielded.m_Count += m_Extra.m_ShieldedOutputs;

	m_Mmr.m_Assets.m_Count = m_DB.ParamIntGetDef(NodeDB::ParamID::AssetsCount);

	bool bUpdateChecksum = !m_DB.ParamGet(NodeDB::ParamID::CfgChecksum, NULL, &blob);
	if (!bUpdateChecksum)
	{
		const HeightHash* pFork = Rules::get().FindFork(hv);
		if (&Rules::get().get_LastFork() != pFork)
		{
			if (!pFork)
			{
				std::ostringstream os;
				os << "Data configuration is incompatible: " << hv;
				throw std::runtime_error(os.str());
			}

			NodeDB::StateID sid;
			m_DB.get_Cursor(sid);

			if (sid.m_Height >= pFork[1].m_Height)
			{
				std::ostringstream os;
				os << "Data configuration: " << hv << ", Fork didn't happen at " << pFork[1].m_Height;
				throw std::runtime_error(os.str());
			}

			bUpdateChecksum = true;
		}
	}

	if (bUpdateChecksum)
	{
		LOG_INFO() << "Settings configuration";

		blob = Blob(Rules::get().get_LastFork().m_Hash);
		m_DB.ParamSet(NodeDB::ParamID::CfgChecksum, NULL, &blob);
	}

	ZeroObject(m_SyncData);

	blob.p = &m_SyncData;
	blob.n = sizeof(m_SyncData);
	m_DB.ParamGet(NodeDB::ParamID::SyncData, nullptr, &blob);

	LogSyncData();

	m_nSizeUtxoComission = 0;

	if (Rules::get().TreasuryChecksum == Zero)
		m_Extra.m_TxosTreasury = 1; // artificial gap
	else
		m_DB.ParamGet(NodeDB::ParamID::Treasury, &m_Extra.m_TxosTreasury, nullptr, nullptr);

	m_DB.get_Cursor(m_Cursor.m_Sid);
	m_Mmr.m_States.m_Count = m_Cursor.m_Sid.m_Height - Rules::HeightGenesis;
	InitCursor(false);

	InitializeUtxos(szPath);

	m_Extra.m_Txos = get_TxosBefore(m_Cursor.m_ID.m_Height + 1);

	m_Horizon.Normalize();

	if (PruneOld() && !sp.m_Vacuum)
	{
		LOG_INFO() << "Old data was just removed from the DB. Some space can be freed by vacuum";
	}

	if (sp.m_Vacuum)
		Vacuum();

	TryGoUp();
}

void NodeProcessor::InitializeUtxos(const char* sz)
{
	if (InitUtxoMapping(sz, false))
	{
		LOG_INFO() << "UTXO image found";
		if (TestDefinition())
			return; // ok

		LOG_WARNING() << "Definition mismatch, discarding UTXO image";
		m_Utxos.Close();
		InitUtxoMapping(sz, true);
	}

	LOG_INFO() << "Rebuilding UTXO image...";
	InitializeUtxos();

	if (!TestDefinition())
	{
		LOG_ERROR() << "Definition mismatch";
		OnCorrupted();
	}
}

bool NodeProcessor::TestDefinition()
{
	if ((m_Cursor.m_ID.m_Height < Rules::HeightGenesis) || (m_Cursor.m_ID.m_Height < m_SyncData.m_TxoLo))
		return true; // irrelevant

	Merkle::Hash hv;
	Evaluator ev(*this);
	ev.get_Definition(hv);

	return m_Cursor.m_Full.m_Definition == hv;
}


// Ridiculous! Had to write this because strmpi isn't standard!
int My_strcmpi(const char* sz1, const char* sz2)
{
	while (true)
	{
		int c1 = std::tolower(*sz1++);
		int c2 = std::tolower(*sz2++);
		if (c1 < c2)
			return -1;
		if (c1 > c2)
			return 1;

		if (!c1)
			break;
	}
	return 0;
}

void NodeProcessor::get_UtxoMappingPath(std::string& sPath, const char* sz)
{
	// derive UTXO path from db path
	sPath = sz;

	static const char szSufix[] = ".db";
	const size_t nSufix = _countof(szSufix) - 1;

	if ((sPath.size() >= nSufix) && !My_strcmpi(sPath.c_str() + sPath.size() - nSufix, szSufix))
		sPath.resize(sPath.size() - nSufix);

	sPath += "-utxo-image.bin";
}

bool NodeProcessor::InitUtxoMapping(const char* sz, bool bForceReset)
{
	// derive UTXO path from db path
	std::string sPath;
	get_UtxoMappingPath(sPath, sz);

	UtxoTreeMapped::Stamp us;
	Blob blob(us);

	// don't use the saved image if no height: we may contain treasury UTXOs, but no way to verify the contents
	if (bForceReset || (m_Cursor.m_ID.m_Height < Rules::HeightGenesis) || !m_DB.ParamGet(NodeDB::ParamID::UtxoStamp, nullptr, &blob))
	{
		us = 1U;
		us.Negate();
	}

	return m_Utxos.Open(sPath.c_str(), us);
}

void NodeProcessor::LogSyncData()
{
	if (!IsFastSync())
		return;

	LOG_INFO() << "Fast-sync mode up to height " << m_SyncData.m_Target.m_Height;
}

void NodeProcessor::SaveSyncData()
{
	if (IsFastSync())
	{
		Blob blob(&m_SyncData, sizeof(m_SyncData));
		m_DB.ParamSet(NodeDB::ParamID::SyncData, nullptr, &blob);
	}
	else
		m_DB.ParamSet(NodeDB::ParamID::SyncData, nullptr, nullptr);
}

NodeProcessor::Mmr::Mmr(NodeDB& db)
	:m_States(db)
	,m_Shielded(db, NodeDB::StreamType::ShieldedMmr, true)
	,m_Assets(db, NodeDB::StreamType::AssetsMmr, true)
{
}

NodeProcessor::NodeProcessor()
	:m_Mmr(m_DB)
{
}

NodeProcessor::~NodeProcessor()
{
	if (m_DbTx.IsInProgress())
	{
		try {
			CommitUtxosAndDB();
		} catch (const CorruptionException& e) {
			LOG_ERROR() << "DB Commit failed: %s" << e.m_sErr;
		}
	}
}

void NodeProcessor::CommitUtxosAndDB()
{
	UtxoTreeMapped::Stamp us;

	bool bFlushUtxos = (m_Utxos.IsOpen() && m_Utxos.get_Hdr().m_Dirty);

	if (bFlushUtxos)
	{
		Blob blob(us);

		if (m_DB.ParamGet(NodeDB::ParamID::UtxoStamp, nullptr, &blob)) {
			ECC::Hash::Processor() << us >> us;
		} else {
			ECC::GenRandom(us);
		}

		m_DB.ParamSet(NodeDB::ParamID::UtxoStamp, nullptr, &blob);
	}

	m_DbTx.Commit();

	if (bFlushUtxos)
		m_Utxos.FlushStrict(us);
}

void NodeProcessor::Vacuum()
{
	if (m_DbTx.IsInProgress())
		m_DbTx.Commit();

	LOG_INFO() << "DB compacting...";
	m_DB.Vacuum();
	LOG_INFO() << "DB compacting completed";

	m_DbTx.Start(m_DB);
}

void NodeProcessor::CommitDB()
{
	if (m_DbTx.IsInProgress())
	{
		CommitUtxosAndDB();
		m_DbTx.Start(m_DB);
	}
}

void NodeProcessor::InitCursor(bool bMovingUp)
{
	if (m_Cursor.m_Sid.m_Height >= Rules::HeightGenesis)
	{
		if (bMovingUp)
		{
			assert(m_Cursor.m_Full.m_Height == m_Cursor.m_Sid.m_Height); // must already initialized
			m_Cursor.m_History = m_Cursor.m_HistoryNext;
		}
		else
		{
			m_DB.get_State(m_Cursor.m_Sid.m_Row, m_Cursor.m_Full);
			m_Mmr.m_States.get_Hash(m_Cursor.m_History);
		}

		m_Cursor.m_Full.get_ID(m_Cursor.m_ID);
		m_Mmr.m_States.get_PredictedHash(m_Cursor.m_HistoryNext, m_Cursor.m_ID.m_Hash);
	}
	else
	{
		m_Mmr.m_States.m_Count = 0;
		ZeroObject(m_Cursor);
		m_Cursor.m_ID.m_Hash = Rules::get().Prehistoric;
	}

	m_Cursor.m_DifficultyNext = get_NextDifficulty();
}

void NodeProcessor::CongestionCache::Clear()
{
	while (!m_lstTips.empty())
		Delete(&m_lstTips.front());
}

void NodeProcessor::CongestionCache::Delete(TipCongestion* pVal)
{
	m_lstTips.erase(TipList::s_iterator_to(*pVal));
	delete pVal;
}

NodeProcessor::CongestionCache::TipCongestion* NodeProcessor::CongestionCache::Find(const NodeDB::StateID& sid)
{
	TipCongestion* pRet = nullptr;

	for (TipList::iterator it = m_lstTips.begin(); m_lstTips.end() != it; it++)
	{
		TipCongestion& x = *it;
		if (!x.IsContained(sid))
			continue;

		// in case of several matches prefer the one with lower height
		if (pRet && (pRet->m_Height <= x.m_Height))
			continue;

		pRet = &x;
	}

	return pRet;
}

bool NodeProcessor::CongestionCache::TipCongestion::IsContained(const NodeDB::StateID& sid)
{
	if (sid.m_Height > m_Height)
		return false;

	Height dh = m_Height - sid.m_Height;
	if (dh >= m_Rows.size())
		return false;

	return (m_Rows.at(dh) == sid.m_Row);
}

NodeProcessor::CongestionCache::TipCongestion* NodeProcessor::EnumCongestionsInternal()
{
	assert(IsTreasuryHandled());

	CongestionCache cc;
	cc.m_lstTips.swap(m_CongestionCache.m_lstTips);

	CongestionCache::TipCongestion* pMaxTarget = nullptr;

	// Find all potentially missing data
	NodeDB::WalkerState ws;
	for (m_DB.EnumTips(ws); ws.MoveNext(); )
	{
		NodeDB::StateID& sid = ws.m_Sid; // alias
		if (NodeDB::StateFlags::Reachable & m_DB.GetStateFlags(sid.m_Row))
			continue;

		Difficulty::Raw wrk;
		m_DB.get_ChainWork(sid.m_Row, wrk);

		if (wrk < m_Cursor.m_Full.m_ChainWork)
			continue; // not interested in tips behind the current cursor

		CongestionCache::TipCongestion* pEntry = nullptr;
		bool bCheckCache = true;
		bool bNeedHdrs = false;

		while (true)
		{
			if (bCheckCache)
			{
				CongestionCache::TipCongestion* p = cc.Find(sid);
				if (p)
				{
					assert(p->m_Height >= sid.m_Height);
					while (p->m_Height > sid.m_Height)
					{
						p->m_Height--;
						p->m_Rows.pop_front();
					}

					if (pEntry)
					{
						for (size_t i = pEntry->m_Rows.size(); i--; p->m_Height++)
							p->m_Rows.push_front(pEntry->m_Rows.at(i));

						m_CongestionCache.Delete(pEntry);
					}

					cc.m_lstTips.erase(CongestionCache::TipList::s_iterator_to(*p));
					m_CongestionCache.m_lstTips.push_back(*p);

					while (NodeDB::StateFlags::Reachable & m_DB.GetStateFlags(p->m_Rows.at(p->m_Rows.size() - 1)))
						p->m_Rows.pop_back(); // already retrieved

					assert(p->m_Rows.size());

					sid.m_Row = p->m_Rows.at(p->m_Rows.size() - 1);
					sid.m_Height = p->m_Height - (p->m_Rows.size() - 1);

					pEntry = p;
					bCheckCache = false;
				}
			}

			if (!pEntry)
			{
				pEntry = new CongestionCache::TipCongestion;
				m_CongestionCache.m_lstTips.push_back(*pEntry);

				pEntry->m_Height = sid.m_Height;
			}

			if (bCheckCache)
			{
				CongestionCache::TipCongestion* p = m_CongestionCache.Find(sid);
				if (p)
				{
					assert(p != pEntry);

					// copy the rest
					for (size_t i = p->m_Height - sid.m_Height; i < p->m_Rows.size(); i++)
						pEntry->m_Rows.push_back(p->m_Rows.at(i));

					sid.m_Row = p->m_Rows.at(p->m_Rows.size() - 1);
					sid.m_Height = p->m_Height - (p->m_Rows.size() - 1);

					bCheckCache = false;
				}
			}

			if (pEntry->m_Height >= sid.m_Height + pEntry->m_Rows.size())
				pEntry->m_Rows.push_back(sid.m_Row);

			if (Rules::HeightGenesis == sid.m_Height)
				break;

			if (!m_DB.get_Prev(sid))
			{
				bNeedHdrs = true;
				break;
			}

			if (NodeDB::StateFlags::Reachable & m_DB.GetStateFlags(sid.m_Row))
				break;
		}

		assert(pEntry && pEntry->m_Rows.size());
		pEntry->m_bNeedHdrs = bNeedHdrs;

		if (!bNeedHdrs && (!pMaxTarget || (pMaxTarget->m_Height < pEntry->m_Height)))
			pMaxTarget = pEntry;
	}

	return pMaxTarget;
}

template <typename T>
bool IsBigger2(T a, T b1, T b2)
{
	b1 += b2;
	return (b1 >= b2) && (a > b1);
}

template <typename T>
bool IsBigger3(T a, T b1, T b2, T b3)
{
	b2 += b3;
	return (b2 >= b3) && IsBigger2(a, b1, b2);
}

void NodeProcessor::EnumCongestions()
{
	if (!IsTreasuryHandled())
	{
		Block::SystemState::ID id;
		ZeroObject(id);
		NodeDB::StateID sidTrg;
		sidTrg.SetNull();

		RequestData(id, true, sidTrg);
		return;
	}

	CongestionCache::TipCongestion* pMaxTarget = EnumCongestionsInternal();

	// Check the fast-sync status
	if (pMaxTarget)
	{
		bool bFirstTime =
			!IsFastSync() &&
			IsBigger3(pMaxTarget->m_Height, m_Cursor.m_ID.m_Height, m_Horizon.m_Sync.Hi, m_Horizon.m_Sync.Hi / 2);

		if (bFirstTime)
		{
			// first time target acquisition
			// TODO - verify the headers w.r.t. difficulty and Chainwork
			m_SyncData.m_h0 = pMaxTarget->m_Height - pMaxTarget->m_Rows.size();

			if (pMaxTarget->m_Height > m_Horizon.m_Sync.Lo)
				m_SyncData.m_TxoLo = pMaxTarget->m_Height - m_Horizon.m_Sync.Lo;

			std::setmax(m_SyncData.m_TxoLo, m_Extra.m_TxoLo);
		}

		// check if the target should be moved fwd
		bool bTrgChange =
			(IsFastSync() || bFirstTime) &&
			IsBigger2(pMaxTarget->m_Height, m_SyncData.m_Target.m_Height, m_Horizon.m_Sync.Hi);

		if (bTrgChange)
		{
			Height hTargetPrev = bFirstTime ? (pMaxTarget->m_Height - pMaxTarget->m_Rows.size()) : m_SyncData.m_Target.m_Height;

			m_SyncData.m_Target.m_Height = pMaxTarget->m_Height - m_Horizon.m_Sync.Hi;
			m_SyncData.m_Target.m_Row = pMaxTarget->m_Rows.at(pMaxTarget->m_Height - m_SyncData.m_Target.m_Height);

			if (m_SyncData.m_TxoLo)
			{
				// ensure no old blocks, which could be generated with incorrect TxLo
				//
				// Deleting all the blocks in the range is a time-consuming operation, whereas it's VERY unlikely there's any block in there
				// So we'll limit the height range by the maximum "sane" value (which is also very unlikely to contain any block).
				//
				// In a worst-case scenario (extremely unlikely) the sync will fail, then all the blocks will be deleted, and sync restarts
				Height hMaxSane = m_Cursor.m_ID.m_Height + Rules::get().MaxRollback;
				if (hTargetPrev < hMaxSane)
				{
					if (m_SyncData.m_Target.m_Height <= hMaxSane)
						DeleteBlocksInRange(m_SyncData.m_Target, hTargetPrev);
					else
					{
						NodeDB::StateID sid;
						sid.m_Height = hMaxSane;
						sid.m_Row = pMaxTarget->m_Rows.at(pMaxTarget->m_Height - hMaxSane);

						DeleteBlocksInRange(sid, hTargetPrev);
					}
				}
			}

			SaveSyncData();
		}

		if (bFirstTime)
			LogSyncData();
	}

	// request missing data
	for (CongestionCache::TipList::iterator it = m_CongestionCache.m_lstTips.begin(); m_CongestionCache.m_lstTips.end() != it; it++)
	{
		CongestionCache::TipCongestion& x = *it;

		if (!(x.m_bNeedHdrs || (&x == pMaxTarget)))
			continue; // current policy - ask only for blocks with the largest proven (wrt headers) chainwork

		Block::SystemState::ID id;

		NodeDB::StateID sidTrg;
		sidTrg.m_Height = x.m_Height;
		sidTrg.m_Row = x.m_Rows.at(0);

		if (!x.m_bNeedHdrs)
		{
			if (IsFastSync() && !x.IsContained(m_SyncData.m_Target))
				continue; // ignore irrelevant branches

			NodeDB::StateID sid;
			sid.m_Height = x.m_Height - (x.m_Rows.size() - 1);
			sid.m_Row = x.m_Rows.at(x.m_Rows.size() - 1);

			m_DB.get_StateID(sid, id);
			RequestDataInternal(id, sid.m_Row, true, sidTrg);
		}
		else
		{
			uint64_t rowid = x.m_Rows.at(x.m_Rows.size() - 1);

			Block::SystemState::Full s;
			m_DB.get_State(rowid, s);

			id.m_Height = s.m_Height - 1;
			id.m_Hash = s.m_Prev;

			RequestDataInternal(id, rowid, false, sidTrg);
		}
	}
}

const uint64_t* NodeProcessor::get_CachedRows(const NodeDB::StateID& sid, Height nCountExtra)
{
	EnumCongestionsInternal();

	CongestionCache::TipCongestion* pVal = m_CongestionCache.Find(sid);
	if (pVal)
	{
		assert(pVal->m_Height >= sid.m_Height);
		Height dh = (pVal->m_Height - sid.m_Height);

		if (pVal->m_Rows.size() > nCountExtra + dh)
			return &pVal->m_Rows.at(dh);
	}
	return nullptr;
}

Height NodeProcessor::get_LowestReturnHeight() const
{
	Height hRet = m_Extra.m_TxoHi;

	Height h0 = IsFastSync() ? m_SyncData.m_h0 : m_Cursor.m_ID.m_Height;
	Height hMaxRollback = Rules::get().MaxRollback;

	if (h0 > hMaxRollback)
	{
		h0 -= hMaxRollback;
		std::setmax(hRet, h0);
	}

	return hRet;
}

void NodeProcessor::RequestDataInternal(const Block::SystemState::ID& id, uint64_t row, bool bBlock, const NodeDB::StateID& sidTrg)
{
	if (id.m_Height >= get_LowestReturnHeight())
	{
		RequestData(id, bBlock, sidTrg);
	}
	else
	{
		LOG_WARNING() << id << " State unreachable!"; // probably will pollute the log, but it's a critical situation anyway
	}
}

struct NodeProcessor::MultiSigmaContext
{
	static const uint32_t s_Chunk = 0x400;

	struct Node
	{
		struct ID
			:public boost::intrusive::set_base_hook<>
		{
			TxoID m_Value;
			bool operator < (const ID& x) const { return (m_Value < x.m_Value); }

			IMPLEMENT_GET_PARENT_OBJ(Node, m_ID)
		} m_ID;

		ECC::Scalar::Native m_pS[s_Chunk];
		uint32_t m_Min, m_Max;

		typedef boost::intrusive::multiset<ID> IDSet;
	};


	std::mutex m_Mutex;
	Node::IDSet m_Set;

	void Add(TxoID id0, uint32_t nCount, const ECC::Scalar::Native*);
	void ClearLocked();

	~MultiSigmaContext()
	{
		ClearLocked();
	}

	void Calculate(ECC::Point::Native&, NodeProcessor&);

private:

	struct MyTask;

	void DeleteRaw(Node&);
	std::vector<ECC::Point::Native> m_vRes;

	virtual Sigma::CmList& get_List() = 0;
	virtual void PrepareList(NodeProcessor&, const Node&) = 0;
};

void NodeProcessor::MultiSigmaContext::ClearLocked()
{
	while (!m_Set.empty())
		DeleteRaw(m_Set.begin()->get_ParentObj());
}

void NodeProcessor::MultiSigmaContext::DeleteRaw(Node& n)
{
	m_Set.erase(Node::IDSet::s_iterator_to(n.m_ID));
	delete &n;
}

void NodeProcessor::MultiSigmaContext::Add(TxoID id0, uint32_t nCount, const ECC::Scalar::Native* pS)
{
	uint32_t nOffset = static_cast<uint32_t>(id0 % s_Chunk);

	Node::ID key;
	key.m_Value = id0 - nOffset;

	std::unique_lock<std::mutex> scope(m_Mutex);

	while (nCount)
	{
		uint32_t nPortion = std::min(nCount, s_Chunk - nOffset);
		
		Node::IDSet::iterator it = m_Set.find(key);
		bool bNew = (m_Set.end() == it);
		if (bNew)
		{
			Node* pN = new Node;
			pN->m_ID = key;
			m_Set.insert(pN->m_ID);
			it = Node::IDSet::s_iterator_to(pN->m_ID);
		}

		Node& n = it->get_ParentObj();
		if (bNew)
		{
			n.m_Min = nOffset;
			n.m_Max = nOffset + nPortion;
		}
		else
		{
			std::setmin(n.m_Min, nOffset);
			std::setmax(n.m_Max, nOffset + nPortion);
		}

		ECC::Scalar::Native* pT = n.m_pS + nOffset;
		for (uint32_t i = 0; i < nPortion; i++)
			pT[i] += pS[i];

		pS += nPortion;
		nCount -= nPortion;
		key.m_Value += s_Chunk;
		nOffset = 0;
	}
}

struct NodeProcessor::MultiSigmaContext::MyTask
	:public Executor::TaskSync
{
	MultiSigmaContext* m_pThis;
	const Node* m_pNode;
	//const ECC::Scalar::Native* m_pS;

	virtual void Exec(Executor::Context& ctx) override
	{
		ECC::Point::Native& val = m_pThis->m_vRes[ctx.m_iThread];
		val = Zero;

		uint32_t i0, nCount;
		ctx.get_Portion(i0, nCount, m_pNode->m_Max - m_pNode->m_Min);
		i0 += m_pNode->m_Min;

		m_pThis->get_List().Calculate(val, i0, nCount, m_pNode->m_pS);
	}
};

void NodeProcessor::MultiSigmaContext::Calculate(ECC::Point::Native& res, NodeProcessor& np)
{
	Executor& ex = np.get_Executor();
	uint32_t nThreads = ex.get_Threads();

	while (!m_Set.empty())
	{
		Node& n = m_Set.begin()->get_ParentObj();
		assert(n.m_Min < n.m_Max);
		assert(n.m_Max <= s_Chunk);

		m_vRes.resize(nThreads);
		PrepareList(np, n);

		MyTask t;
		t.m_pThis = this;
		t.m_pNode = &n;

		ex.ExecAll(t);

		for (uint32_t i = 0; i < nThreads; i++)
			res += m_vRes[i];

		DeleteRaw(n);
	}
}

struct NodeProcessor::MultiShieldedContext
	:public NodeProcessor::MultiSigmaContext
{
	bool IsValid(const TxVectors::Eternal&, ECC::InnerProduct::BatchContext&, uint32_t iVerifier, uint32_t nTotal);
private:

	Sigma::CmListVec m_Lst;

	bool IsValid(const TxKernelShieldedInput&, std::vector<ECC::Scalar::Native>& vBuf, ECC::InnerProduct::BatchContext&);

	virtual Sigma::CmList& get_List() override
	{
		return m_Lst;
	}

	virtual void PrepareList(NodeProcessor& np, const Node& n) override
	{
		m_Lst.m_vec.resize(s_Chunk); // will allocate if empty
		np.get_DB().ShieldedRead(n.m_ID.m_Value + n.m_Min, &m_Lst.m_vec.front() + n.m_Min, n.m_Max - n.m_Min);
	}
};

bool NodeProcessor::MultiShieldedContext::IsValid(const TxKernelShieldedInput& krn, std::vector<ECC::Scalar::Native>& vKs, ECC::InnerProduct::BatchContext& bc)
{
	const Lelantus::Proof& x = krn.m_SpendProof;
	uint32_t N = x.m_Cfg.get_N();
	if (!N)
		return false;

	vKs.resize(N);
	memset0(&vKs.front(), sizeof(ECC::Scalar::Native) * N);

	ECC::Point::Native hGen;
	if (krn.m_pAsset)
		BEAM_VERIFY(hGen.Import(krn.m_pAsset->m_hGen)); // must already be tested in krn.IsValid();

	ECC::Oracle oracle;
	oracle << krn.m_Msg;
	if (!x.IsValid(bc, oracle, &vKs.front(), &hGen))
		return false;

	TxoID id1 = krn.m_WindowEnd;
	if (id1 >= N)
		Add(id1 - N, N, &vKs.front());
	else
		Add(0, static_cast<uint32_t>(id1), &vKs.front() + N - static_cast<uint32_t>(id1));

	return true;
}

bool NodeProcessor::MultiShieldedContext::IsValid(const TxVectors::Eternal& txve, ECC::InnerProduct::BatchContext& bc, uint32_t iVerifier, uint32_t nTotal)
{
	struct Walker
		:public TxKernel::IWalker
	{
		std::vector<ECC::Scalar::Native> m_vKs;
		MultiShieldedContext* m_pThis;
		ECC::InnerProduct::BatchContext* m_pBc;
		uint32_t m_iVerifier;
		uint32_t m_Total;

		virtual bool OnKrn(const TxKernel& krn) override
		{
			if (TxKernel::Subtype::ShieldedInput != krn.get_Subtype())
				return true;

			const TxKernelShieldedInput& v = Cast::Up<TxKernelShieldedInput>(krn);

			if (!m_iVerifier && !m_pThis->IsValid(v, m_vKs, *m_pBc))
				return false;

			if (++m_iVerifier == m_Total)
				m_iVerifier = 0;

			return true;
		}

	} wlk;
	wlk.m_pThis = this;
	wlk.m_pBc = &bc;
	wlk.m_iVerifier = iVerifier;
	wlk.m_Total = nTotal;

	return wlk.Process(txve.m_vKernels);
}

struct NodeProcessor::MultiAssetContext
	:public NodeProcessor::MultiSigmaContext
{
	struct BatchCtx
		:public Asset::Proof::BatchContext
	{
		MultiAssetContext& m_Ctx;
		BatchCtx(MultiAssetContext& ctx) :m_Ctx(ctx) {}

		std::vector<ECC::Scalar::Native> m_vKs;

		virtual bool IsValid(ECC::Point::Native& hGen, const Asset::Proof&) override;
	};

private:

	Asset::Proof::CmList m_Lst;

	virtual Sigma::CmList& get_List() override
	{
		return m_Lst;
	}

	virtual void PrepareList(NodeProcessor& np, const Node& n) override
	{
		static_assert(sizeof(n.m_ID.m_Value) >= sizeof(m_Lst.m_Begin));

		// TODO: maybe cache it in DB
		m_Lst.m_Begin = static_cast<Asset::ID>(n.m_ID.m_Value);
	}
};

bool NodeProcessor::MultiAssetContext::BatchCtx::IsValid(ECC::Point::Native& hGen, const Asset::Proof& p)
{
	assert(ECC::InnerProduct::BatchContext::s_pInstance);
	ECC::InnerProduct::BatchContext& bc = *ECC::InnerProduct::BatchContext::s_pInstance;

	const Sigma::Cfg& cfg = Rules::get().CA.m_ProofCfg;
	uint32_t N = cfg.get_N();
	assert(N);

	m_vKs.resize(N); // will allocate if empty
	memset0(&m_vKs.front(), sizeof(ECC::Scalar::Native) * N);

	if (!p.IsValid(hGen, bc, &m_vKs.front()))
		return false;

	m_Ctx.Add(p.m_Begin, N, &m_vKs.front());
	return true;
}

struct NodeProcessor::MultiblockContext
{
	NodeProcessor& m_This;

	std::mutex m_Mutex;

	TxoID m_id0;
	HeightRange m_InProgress;
	PeerID  m_pidLast;

	MultiblockContext(NodeProcessor& np)
		:m_This(np)
	{
		m_InProgress.m_Max = m_This.m_Cursor.m_ID.m_Height;
		m_InProgress.m_Min = m_InProgress.m_Max + 1;
		assert(m_InProgress.IsEmpty());

		m_id0 = m_This.get_TxosBefore(m_This.m_SyncData.m_h0 + 1); // inputs of blocks below TxLo must be before this

		if (m_This.IsFastSync())
			m_Sigma.Import(m_This.m_SyncData.m_Sigma);
	}

	~MultiblockContext()
	{
		m_This.get_Executor().Flush();

		if (m_bBatchDirty)
		{
			// make sure we don't leave batch context in an invalid state
			struct Task0 :public Executor::TaskSync {
				virtual void Exec(Executor::Context&) override
				{
					ECC::InnerProduct::BatchContext* pBc = ECC::InnerProduct::BatchContext::s_pInstance;
					if (pBc)
						pBc->Reset();
				}
			};

			Task0 t;
			m_This.get_Executor().ExecAll(t);
		}
	}

	ECC::Scalar::Native m_Offset;
	ECC::Point::Native m_Sigma;

	MultiShieldedContext m_Msc;
	MultiAssetContext m_Mac;

	size_t m_SizePending = 0;
	bool m_bFail = false;
	bool m_bBatchDirty = false;

	struct MyTask
		:public Executor::TaskAsync
	{
		virtual void Exec(Executor::Context&) override;
		virtual ~MyTask() {}

		struct Shared
		{
			typedef std::shared_ptr<Shared> Ptr;

			MultiblockContext& m_Mbc;
			uint32_t m_Done;

			Shared(MultiblockContext& mbc)
				:m_Mbc(mbc)
				,m_Done(0)
			{
			}

			virtual ~Shared() {} // auto

			virtual void Exec(uint32_t iVerifier) = 0;
		};

		struct SharedBlock
			:public Shared
		{
			typedef std::shared_ptr<SharedBlock> Ptr;

			Block::Body m_Body;
			size_t m_Size;
			TxBase::Context::Params m_Pars;
			TxBase::Context m_Ctx;

			SharedBlock(MultiblockContext& mbc)
				:Shared(mbc)
				,m_Ctx(m_Pars)
			{
			}

			virtual ~SharedBlock() {} // auto

			virtual void Exec(uint32_t iVerifier) override;
		};

		Shared::Ptr m_pShared;
		uint32_t m_iVerifier;
	};

	bool Flush()
	{
		FlushInternal();
		return !m_bFail;
	}

	void FlushInternal()
	{
		if (m_bFail || m_InProgress.IsEmpty())
			return;

		Executor& ex = m_This.get_Executor();
		ex.Flush();

		if (m_bFail)
			return;

		if (m_bBatchDirty)
		{
			struct Task1 :public Executor::TaskSync
			{
				MultiblockContext* m_pMbc;
				ECC::Point::Native* m_pBatchSigma;
				virtual void Exec(Executor::Context&) override
				{
					ECC::InnerProduct::BatchContext* pBc = ECC::InnerProduct::BatchContext::s_pInstance;
					if (pBc && !pBc->Flush())
					{
						{
							std::unique_lock<std::mutex> scope(m_pMbc->m_Mutex);
							(*m_pBatchSigma) += pBc->m_Sum;

						}
						pBc->m_Sum = Zero;
					}
				}
			};

			ECC::Point::Native ptBatchSigma;

			Task1 t;
			t.m_pMbc = this;
			t.m_pBatchSigma = &ptBatchSigma;
			ex.ExecAll(t);
			assert(!m_bFail);
			m_bBatchDirty = false;

			m_Msc.Calculate(ptBatchSigma, m_This);
			m_Mac.Calculate(ptBatchSigma, m_This);

			if (!(ptBatchSigma == Zero))
			{
				m_bFail = true;
				return;
			}
		}

		if (m_This.IsFastSync())
		{
			if (!(m_Offset == Zero))
			{
				ECC::Mode::Scope scopeFast(ECC::Mode::Fast);

				m_Sigma += ECC::Context::get().G * m_Offset;
				m_Offset = Zero;
			}

			if (m_InProgress.m_Max == m_This.m_SyncData.m_TxoLo)
			{
				// finalize multi-block arithmetics
				TxBase::Context::Params pars;
				pars.m_bAllowUnsignedOutputs = true; // ignore verification of locked coinbase

				TxBase::Context ctx(pars);
				ctx.m_Height.m_Min = m_This.m_SyncData.m_h0 + 1;
				ctx.m_Height.m_Max = m_This.m_SyncData.m_TxoLo;

				ctx.m_Sigma = m_Sigma;

				if (!ctx.IsValidBlock())
				{
					m_bFail = true;
					OnFastSyncFailedOnLo();

					return;
				}

				m_Sigma = Zero;
			}

			m_Sigma.Export(m_This.m_SyncData.m_Sigma);
			m_This.SaveSyncData();
		}
		else
		{
			assert(m_Offset == Zero);
			assert(m_Sigma == Zero);
		}

		m_InProgress.m_Min = m_InProgress.m_Max + 1;
	}

	void OnBlock(const PeerID& pid, const MyTask::SharedBlock::Ptr& pShared)
	{
		assert(pShared->m_Ctx.m_Height.m_Min == pShared->m_Ctx.m_Height.m_Max);
		assert(pShared->m_Ctx.m_Height.m_Min == m_This.m_Cursor.m_ID.m_Height + 1);

		if (m_bFail)
			return;

		bool bMustFlush =
			!m_InProgress.IsEmpty() &&
			(
				(m_pidLast != pid) || // PeerID changed
				(m_InProgress.m_Max == m_This.m_SyncData.m_TxoLo) // range complete up to TxLo
			);

		if (bMustFlush && !Flush())
			return;

		m_pidLast = pid;

		const size_t nSizeMax = 1024 * 1024 * 10; // fair enough

		Executor& ex = m_This.get_Executor();
		for (uint32_t nTasks = static_cast<uint32_t>(-1); ; )
		{
			{
				std::unique_lock<std::mutex> scope(m_Mutex);
				if (m_SizePending <= nSizeMax)
				{
					m_SizePending += pShared->m_Size;
					break;
				}
			}

			assert(nTasks);
			nTasks = ex.Flush(nTasks - 1);
		}

		m_InProgress.m_Max++;
		assert(m_InProgress.m_Max == pShared->m_Ctx.m_Height.m_Min);

		bool bFull = (pShared->m_Ctx.m_Height.m_Min > m_This.m_SyncData.m_Target.m_Height);

		pShared->m_Pars.m_bAllowUnsignedOutputs = !bFull;
		pShared->m_Pars.m_pAbort = &m_bFail;
		pShared->m_Pars.m_nVerifiers = ex.get_Threads();

		PushTasks(pShared, pShared->m_Pars);
	}

	void PushTasks(const MyTask::Shared::Ptr& pShared, TxBase::Context::Params& pars)
	{
		Executor& ex = m_This.get_Executor();
		m_bBatchDirty = true;

		pars.m_pAbort = &m_bFail;
		pars.m_nVerifiers = ex.get_Threads();

		for (uint32_t i = 0; i < pars.m_nVerifiers; i++)
		{
			std::unique_ptr<MyTask> pTask(new MyTask);
			pTask->m_pShared = pShared;
			pTask->m_iVerifier = i;
			ex.Push(std::move(pTask));
		}
	}

	void OnFastSyncFailed(bool bDeleteBlocks)
	{
		// rapid rollback
		m_This.RollbackTo(m_This.m_SyncData.m_h0);
		m_InProgress.m_Max = m_This.m_Cursor.m_ID.m_Height;
		m_InProgress.m_Min = m_InProgress.m_Max + 1;

		if (bDeleteBlocks)
			m_This.DeleteBlocksInRange(m_This.m_SyncData.m_Target, m_This.m_SyncData.m_h0);

		m_This.m_SyncData.m_Sigma = Zero;

		if (m_This.m_SyncData.m_TxoLo > m_This.m_SyncData.m_h0)
		{
			LOG_INFO() << "Retrying with lower TxLo";
			m_This.m_SyncData.m_TxoLo = m_This.m_SyncData.m_h0;
		}
		else {
			LOG_WARNING() << "TxLo already low";
		}

		m_This.SaveSyncData();

		m_pidLast = Zero; // don't blame the last peer for the failure!
	}

	void OnFastSyncFailedOnLo()
	{
		// probably problem in lower blocks
		LOG_WARNING() << "Fast-sync failed on first above-TxLo block.";
		m_pidLast = Zero; // don't blame the last peer
		OnFastSyncFailed(true);
	}
};

void NodeProcessor::MultiblockContext::MyTask::Exec(Executor::Context&)
{
	MultiAssetContext::BatchCtx bcAssets(m_pShared->m_Mbc.m_Mac);
	Asset::Proof::BatchContext::Scope scopeAssets(bcAssets);

	m_pShared->Exec(m_iVerifier);
}

void NodeProcessor::MultiblockContext::MyTask::SharedBlock::Exec(uint32_t iVerifier)
{
	TxBase::Context ctx(m_Ctx.m_Params);
	ctx.m_Height = m_Ctx.m_Height;
	ctx.m_iVerifier = iVerifier;

	bool bSparse = (m_Ctx.m_Height.m_Min <= m_Mbc.m_This.m_SyncData.m_TxoLo);

	beam::TxBase txbDummy;
	if (bSparse)
		txbDummy.m_Offset = Zero;

	bool bValid = ctx.ValidateAndSummarize(bSparse ? txbDummy : m_Body, m_Body.get_Reader());

	if (bValid)
		bValid = m_Mbc.m_Msc.IsValid(m_Body, *ECC::InnerProduct::BatchContext::s_pInstance, iVerifier, m_Ctx.m_Params.m_nVerifiers);

	std::unique_lock<std::mutex> scope(m_Mbc.m_Mutex);

	if (bValid)
		bValid = m_Ctx.Merge(ctx);

	assert(m_Done < m_Pars.m_nVerifiers);
	if (++m_Done == m_Pars.m_nVerifiers)
	{
		assert(m_Mbc.m_SizePending >= m_Size);
		m_Mbc.m_SizePending -= m_Size;

		if (bValid && !bSparse)
			bValid = m_Ctx.IsValidBlock();

		if (bValid && bSparse)
		{
			m_Mbc.m_Offset += m_Body.m_Offset;
			m_Mbc.m_Sigma += m_Ctx.m_Sigma;
		}
	}

	if (!bValid)
		m_Mbc.m_bFail = true;
}

void NodeProcessor::TryGoUp()
{
	if (!IsTreasuryHandled())
		return;

	bool bDirty = false;
	uint64_t rowid = m_Cursor.m_Sid.m_Row;

	while (true)
	{
		NodeDB::StateID sidTrg;

		{
			NodeDB::WalkerState ws;
			m_DB.EnumFunctionalTips(ws);

			if (!ws.MoveNext())
			{
				assert(!m_Cursor.m_Sid.m_Row);
				break; // nowhere to go
			}

			sidTrg = ws.m_Sid;

			Difficulty::Raw wrkTrg;
			m_DB.get_ChainWork(sidTrg.m_Row, wrkTrg);

			assert(wrkTrg >= m_Cursor.m_Full.m_ChainWork);
			if (wrkTrg == m_Cursor.m_Full.m_ChainWork)
				break; // already at maximum (though maybe at different tip)
		}

		TryGoTo(sidTrg);
		bDirty = true;
	}

	if (bDirty)
	{
		PruneOld();
		if (m_Cursor.m_Sid.m_Row != rowid)
			OnNewState();
	}
}

void NodeProcessor::TryGoTo(NodeDB::StateID& sidTrg)
{
	// Calculate the path
	std::vector<uint64_t> vPath;
	while (true)
	{
		vPath.push_back(sidTrg.m_Row);

		if (!m_DB.get_Prev(sidTrg))
		{
			sidTrg.SetNull();
			break;
		}

		if (NodeDB::StateFlags::Active & m_DB.GetStateFlags(sidTrg.m_Row))
			break;
	}

	RollbackTo(sidTrg.m_Height);

	MultiblockContext mbc(*this);
	bool bContextFail = false, bKeepBlocks = false;

	NodeDB::StateID sidFwd = m_Cursor.m_Sid;

	size_t iPos = vPath.size();
	while (iPos)
	{
		sidFwd.m_Height = m_Cursor.m_Sid.m_Height + 1;
		sidFwd.m_Row = vPath[--iPos];

		Block::SystemState::Full s;
		m_DB.get_State(sidFwd.m_Row, s); // need it for logging anyway

		if (!HandleBlock(sidFwd, s, mbc))
		{
			bContextFail = mbc.m_bFail = true;

			if (m_Cursor.m_ID.m_Height + 1 == m_SyncData.m_TxoLo)
				mbc.OnFastSyncFailedOnLo();

			break;
		}

		// Update mmr and cursor
		if (m_Cursor.m_ID.m_Height >= Rules::HeightGenesis)
			m_Mmr.m_States.Append(m_Cursor.m_ID.m_Hash);

		m_DB.MoveFwd(sidFwd);
		m_Cursor.m_Sid = sidFwd;
		m_Cursor.m_Full = s;
		InitCursor(true);

		if (IsFastSync())
			m_DB.DelStateBlockPP(sidFwd.m_Row); // save space

		if (mbc.m_InProgress.m_Max == m_SyncData.m_Target.m_Height)
		{
			if (!mbc.Flush())
				break;

			OnFastSyncOver(mbc, bContextFail);

			if (mbc.m_bFail)
				bKeepBlocks = true;
		}

		if (mbc.m_bFail)
			break;
	}

	if (mbc.Flush())
		return; // at position

	if (!bContextFail)
		LOG_WARNING() << "Context-free verification failed";

	RollbackTo(mbc.m_InProgress.m_Min - 1);

	if (bKeepBlocks)
		return;

	if (!(mbc.m_pidLast == Zero))
	{
		OnPeerInsane(mbc.m_pidLast);

		// delete all the consequent blocks from this peer
		for (; iPos; iPos--)
		{
			PeerID pid;
			if (!m_DB.get_Peer(vPath[iPos - 1], pid))
				break;

			if (pid != mbc.m_pidLast)
				break;

			sidFwd.m_Row = vPath[iPos - 1];
			sidFwd.m_Height++;
		}
	}

	LOG_INFO() << "Deleting blocks range: " << (m_Cursor.m_Sid.m_Height + 1) << "-" <<  sidFwd.m_Height;

	DeleteBlocksInRange(sidFwd, m_Cursor.m_Sid.m_Height);
}

void NodeProcessor::OnFastSyncOver(MultiblockContext& mbc, bool& bContextFail)
{
	assert(mbc.m_InProgress.m_Max == m_SyncData.m_Target.m_Height);

	mbc.m_pidLast = Zero; // don't blame the last peer if something goes wrong
	NodeDB::StateID sidFail;
	sidFail.SetNull(); // suppress warning

	{
		// ensure no reduced UTXOs are left
		NodeDB::WalkerTxo wlk;
		for (m_DB.EnumTxos(wlk, mbc.m_id0); wlk.MoveNext(); )
		{
			if (wlk.m_SpendHeight != MaxHeight)
				continue;

			if (TxoIsNaked(wlk.m_Value))
			{
				bContextFail = mbc.m_bFail = true;
				m_DB.FindStateByTxoID(sidFail, wlk.m_ID);
				break;
			}
		}
	}

	if (mbc.m_bFail)
	{
		LOG_WARNING() << "Fast-sync failed";

		if (!m_DB.get_Peer(sidFail.m_Row, mbc.m_pidLast))
			mbc.m_pidLast = Zero;

		if (m_SyncData.m_TxoLo > m_SyncData.m_h0)
		{
			mbc.OnFastSyncFailed(true);
		}
		else
		{
			// try to preserve blocks, recover them from the TXOs.

			ByteBuffer bbP, bbE;
			while (m_Cursor.m_Sid.m_Height > m_SyncData.m_h0)
			{
				NodeDB::StateID sid = m_Cursor.m_Sid;

				bbP.clear();
				if (!GetBlock(sid, &bbE, &bbP, m_SyncData.m_h0, m_SyncData.m_TxoLo, m_SyncData.m_Target.m_Height, true))
					OnCorrupted();

				if (sidFail.m_Height == sid.m_Height)
				{
					bbP.clear();
					m_DB.SetStateNotFunctional(sid.m_Row);
				}

				RollbackTo(sid.m_Height - 1);

				PeerID peer;
				if (!m_DB.get_Peer(sid.m_Row, peer))
					peer = Zero;

				m_DB.SetStateBlock(sid.m_Row, bbP, bbE, peer);
				m_DB.set_StateTxosAndExtra(sid.m_Row, nullptr, nullptr, nullptr);
			}

			mbc.OnFastSyncFailed(false);
		}
	}
	else
	{
		LOG_INFO() << "Fast-sync succeeded";

		// raise fossil height, hTxoLo, hTxoHi
		RaiseFossil(m_Cursor.m_ID.m_Height);
		RaiseTxoHi(m_Cursor.m_ID.m_Height);
		RaiseTxoLo(m_SyncData.m_TxoLo);

		ZeroObject(m_SyncData);
		SaveSyncData();
	}
}

void NodeProcessor::DeleteBlocksInRange(const NodeDB::StateID& sidTop, Height hStop)
{
	for (NodeDB::StateID sid = sidTop; sid.m_Height > hStop; )
	{
		DeleteBlock(sid.m_Row);

		if (!m_DB.get_Prev(sid))
			sid.SetNull();
	}
}

void NodeProcessor::DeleteBlock(uint64_t row)
{
	m_DB.DelStateBlockAll(row);
	m_DB.SetStateNotFunctional(row);
}

Height NodeProcessor::PruneOld()
{
	if (IsFastSync())
		return 0; // don't remove anything while in fast-sync mode

	Height hRet = 0;

	if (m_Cursor.m_Sid.m_Height > m_Horizon.m_Branching + Rules::HeightGenesis - 1)
	{
		Height h = m_Cursor.m_Sid.m_Height - m_Horizon.m_Branching;

		while (true)
		{
			uint64_t rowid;
			{
				NodeDB::WalkerState ws;
				m_DB.EnumTips(ws);
				if (!ws.MoveNext())
					break;
				if (ws.m_Sid.m_Height >= h)
					break;

				rowid = ws.m_Sid.m_Row;
			}

			do
			{
				if (!m_DB.DeleteState(rowid, rowid))
					break;
				hRet++;

			} while (rowid);
		}
	}

	if (IsBigger2(m_Cursor.m_Sid.m_Height, m_Extra.m_Fossil, (Height) Rules::get().MaxRollback))
		hRet += RaiseFossil(m_Cursor.m_Sid.m_Height - Rules::get().MaxRollback);

	if (IsBigger2(m_Cursor.m_Sid.m_Height, m_Extra.m_TxoLo, m_Horizon.m_Local.Lo))
		hRet += RaiseTxoLo(m_Cursor.m_Sid.m_Height - m_Horizon.m_Local.Lo);

	if (IsBigger2(m_Cursor.m_Sid.m_Height, m_Extra.m_TxoHi, m_Horizon.m_Local.Hi))
		hRet += RaiseTxoHi(m_Cursor.m_Sid.m_Height - m_Horizon.m_Local.Hi);

	return hRet;
}

Height NodeProcessor::RaiseFossil(Height hTrg)
{
	if (hTrg <= m_Extra.m_Fossil)
		return 0;

	Height hRet = 0;

	while (m_Extra.m_Fossil < hTrg)
	{
		m_Extra.m_Fossil++;

		NodeDB::WalkerState ws;
		for (m_DB.EnumStatesAt(ws, m_Extra.m_Fossil); ws.MoveNext(); )
		{
			if (NodeDB::StateFlags::Active & m_DB.GetStateFlags(ws.m_Sid.m_Row))
				m_DB.DelStateBlockPPR(ws.m_Sid.m_Row);
			else
				DeleteBlock(ws.m_Sid.m_Row);

			hRet++;
		}

	}

	m_DB.ParamIntSet(NodeDB::ParamID::FossilHeight, m_Extra.m_Fossil);
	return hRet;
}

Height NodeProcessor::RaiseTxoLo(Height hTrg)
{
	if (hTrg <= m_Extra.m_TxoLo)
		return 0;

	Height hRet = 0;
	std::vector<NodeDB::StateInput> v;

	while (m_Extra.m_TxoLo < hTrg)
	{
		uint64_t rowid = FindActiveAtStrict(++m_Extra.m_TxoLo);
		if (!m_DB.get_StateInputs(rowid, v))
			continue;

		size_t iRes = 0;
		for (size_t i = 0; i < v.size(); i++)
		{
			const NodeDB::StateInput& inp = v[i];
			TxoID id = inp.get_ID();
			if (id >= m_Extra.m_TxosTreasury)
				m_DB.TxoDel(id);
			else
			{
				if (iRes != i)
					v[iRes] = inp;
				iRes++;
			}
		}

		hRet += (v.size() - iRes);

		m_DB.set_StateInputs(rowid, &v.front(), iRes);
	}

	m_Extra.m_TxoLo = hTrg;
	m_DB.ParamIntSet(NodeDB::ParamID::HeightTxoLo, m_Extra.m_TxoLo);

	return hRet;
}

Height NodeProcessor::RaiseTxoHi(Height hTrg)
{
	if (hTrg <= m_Extra.m_TxoHi)
		return 0;

	Height hRet = 0;
	std::vector<NodeDB::StateInput> v;

	NodeDB::WalkerTxo wlk;

	while (m_Extra.m_TxoHi < hTrg)
	{
		uint64_t rowid = FindActiveAtStrict(++m_Extra.m_TxoHi);
		m_DB.get_StateInputs(rowid, v);

		for (size_t i = 0; i < v.size(); i++)
		{
			TxoID id = v[i].get_ID();

			m_DB.TxoGetValue(wlk, id);

			if (TxoIsNaked(wlk.m_Value))
				continue; //?!

			uint8_t pNaked[s_TxoNakedMax];
			TxoToNaked(pNaked, wlk.m_Value);

			m_DB.TxoSetValue(id, wlk.m_Value);
			hRet++;
		}
	}

	m_DB.ParamIntSet(NodeDB::ParamID::HeightTxoHi, m_Extra.m_TxoHi);

	return hRet;
}

void NodeProcessor::TxoToNaked(uint8_t* pBuf, Blob& v)
{
	if (v.n < s_TxoNakedMin)
		OnCorrupted();

	const uint8_t* pSrc = reinterpret_cast<const uint8_t*>(v.p);
	v.p = pBuf;

	if (!(0x10 & pSrc[0]))
	{
		// simple case - just remove some flags and truncate.
		memcpy(pBuf, pSrc, s_TxoNakedMin);
		v.n = s_TxoNakedMin;
		pBuf[0] &= 3;

		return;
	}

	// complex case - the UTXO has Incubation period. Utxo must be re-read
	Deserializer der;
	der.reset(pSrc, v.n);

	Output outp;
	der & outp;

	outp.m_pConfidential.reset();
	outp.m_pPublic.reset();
	outp.m_pAsset.reset();

	StaticBufferSerializer<s_TxoNakedMax> ser;
	ser & outp;

	SerializeBuffer sb = ser.buffer();
	assert(sb.second <= s_TxoNakedMax);

	memcpy(pBuf, sb.first, sb.second);
	v.n = static_cast<uint32_t>(sb.second);
}

bool NodeProcessor::TxoIsNaked(const Blob& v)
{
	if (v.n < s_TxoNakedMin)
		OnCorrupted();

	const uint8_t* pSrc = reinterpret_cast<const uint8_t*>(v.p);

	return !(pSrc[0] & 0xc);

}

NodeProcessor::Evaluator::Evaluator(NodeProcessor& p)
	:m_Proc(p)
{
	m_Height = m_Proc.m_Cursor.m_ID.m_Height;
}

bool NodeProcessor::Evaluator::get_History(Merkle::Hash& hv)
{
	const Cursor& c = m_Proc.m_Cursor;
	hv = (m_Height == c.m_ID.m_Height) ? c.m_History : c.m_HistoryNext;
	return true;
}

bool NodeProcessor::Evaluator::get_Utxos(Merkle::Hash& hv)
{
	m_Proc.m_Utxos.get_Hash(hv);
	return true;
}

bool NodeProcessor::Evaluator::get_Shielded(Merkle::Hash& hv)
{
	m_Proc.m_Mmr.m_Shielded.get_Hash(hv);
	return true;
}

bool NodeProcessor::Evaluator::get_Assets(Merkle::Hash& hv)
{
	m_Proc.m_Mmr.m_Assets.get_Hash(hv);
	return true;
}

void NodeProcessor::ProofBuilder::OnProof(Merkle::Hash& hv, bool bNewOnRight)
{
	m_Proof.emplace_back();
	m_Proof.back().first = bNewOnRight;
	m_Proof.back().second = hv;
}

void NodeProcessor::ProofBuilderHard::OnProof(Merkle::Hash& hv, bool bNewOnRight)
{
	m_Proof.emplace_back();
	m_Proof.back() = hv;
}

uint64_t NodeProcessor::ProcessKrnMmr(Merkle::Mmr& mmr, std::vector<TxKernel::Ptr>& vKrn, const Merkle::Hash& idKrn, TxKernel::Ptr* ppRes)
{
	uint64_t iRet = uint64_t (-1);

	for (size_t i = 0; i < vKrn.size(); i++)
	{
		TxKernel::Ptr& p = vKrn[i];
		const Merkle::Hash& hv = p->m_Internal.m_ID;
		mmr.Append(hv);

		if (hv == idKrn)
		{
			iRet = i; // found
			if (ppRes)
				ppRes->swap(p);
		}
	}

	return iRet;
}

Height NodeProcessor::get_ProofKernel(Merkle::Proof& proof, TxKernel::Ptr* ppRes, const Merkle::Hash& idKrn)
{
	Height h = m_DB.FindKernel(idKrn);
	if (h < Rules::HeightGenesis)
		return h;

	uint64_t rowid = FindActiveAtStrict(h);

	ByteBuffer bbE;
	m_DB.GetStateBlock(rowid, nullptr, &bbE, nullptr);

	TxVectors::Eternal txve;

	Deserializer der;
	der.reset(bbE);
	der & txve;

	Merkle::FixedMmr mmr;
	mmr.Resize(txve.m_vKernels.size());
	size_t iTrg = ProcessKrnMmr(mmr, txve.m_vKernels, idKrn, ppRes);

	if (uint64_t(-1) == iTrg)
		OnCorrupted();

	mmr.get_Proof(proof, iTrg);
	return h;
}

struct NodeProcessor::BlockInterpretCtx
{
	Height m_Height;
	bool m_Fwd;
	bool m_ValidateOnly = false; // don't make changes to state
	bool m_AlreadyValidated = false; // set during reorgs, when a block is being applied for 2nd time
	bool m_SaveKid = true;
	bool m_UpdateMmrs = true;
	bool m_StoreShieldedOutput = false;
	bool m_LimitExceeded = false;

	uint32_t m_ShieldedIns = 0;
	uint32_t m_ShieldedOuts = 0;
	Asset::ID m_AssetsUsed = Asset::s_MaxCount + 1;
	Asset::ID m_AssetHi = static_cast<Asset::ID>(-1); // last valid Asset ID

	// rollback data
	ByteBuffer* m_pRollback = nullptr;

	struct Ser
		:public Serializer
	{
		typedef uintBigFor<uint32_t>::Type Marker;

		BlockInterpretCtx& m_This;
		size_t m_Pos;

		Ser(BlockInterpretCtx&);
		~Ser();
	};

	struct Der
		:public Deserializer
	{
		Der(BlockInterpretCtx&);
	private:
		void SetBwd(ByteBuffer&, uint32_t nPortion);
	};

	struct BlobItem
		:public boost::intrusive::set_base_hook<>
	{
		uint32_t m_Size;
#ifdef _MSC_VER
#	pragma warning (disable: 4200) // 0-sized array
#endif // _MSC_VER
		uint8_t m_pBuf[0]; // var size
#ifdef _MSC_VER
#	pragma warning (default: 4200)
#endif // _MSC_VER

		Blob ToBlob() const
		{
			Blob x;
			x.p = m_pBuf;
			x.n = m_Size;
			return x;
		}

		bool operator < (const BlobItem& x) const {
			return ToBlob() < x.ToBlob();
		}

		void* operator new(size_t n, uint32_t nExtra) {
			return new uint8_t[n + nExtra];
		}

		void operator delete(void* p) {
			delete[] reinterpret_cast<uint8_t*>(p);
		}

		void operator delete(void* p, uint32_t) {
			delete[] reinterpret_cast<uint8_t*>(p);
		}
	};

	struct BlobSet
		:public boost::intrusive::multiset<BlobItem>
	{
		~BlobSet();
		void Clear();

		bool Find(const Blob&) const;
		void Add(const Blob&);
	};

	BlobSet* m_pDups = nullptr; // used in validate-only mode

	typedef std::set<Blob> BlobPtrSet; // like BlobSet, but buffers are not allocated/copied
	BlobPtrSet* m_pDupIDs;

	BlockInterpretCtx(Height h, bool bFwd)
		:m_Height(h)
		,m_Fwd(bFwd)
	{
	}

	void SetAssetHi(const NodeProcessor& np)
	{
		m_AssetHi = static_cast<Asset::ID>(np.m_Mmr.m_Assets.m_Count);
	}

	bool ValidateAssetRange(const Asset::Proof::Ptr& p) const
	{
		return !p || (p->m_Begin <= m_AssetHi);
	}

	void EnsureAssetsUsed(NodeDB&);
};

bool NodeProcessor::HandleTreasury(const Blob& blob)
{
	assert(!IsTreasuryHandled());

	Deserializer der;
	der.reset(blob.p, blob.n);
	Treasury::Data td;

	try {
		der & td;
	} catch (const std::exception&) {
		LOG_WARNING() << "Treasury corrupt";
		return false;
	}

	if (!td.IsValid())
	{
		LOG_WARNING() << "Treasury validation failed";
		return false;
	}

	std::vector<Treasury::Data::Burst> vBursts = td.get_Bursts();

	std::ostringstream os;
	os << "Treasury check. Total bursts=" << vBursts.size();

	for (size_t i = 0; i < vBursts.size(); i++)
	{
		const Treasury::Data::Burst& b = vBursts[i];
		os << "\n\t" << "Height=" << b.m_Height << ", Value=" << b.m_Value;
	}

	LOG_INFO() << os.str();

	BlockInterpretCtx bic(0, true);
	bic.SetAssetHi(*this);
	for (size_t iG = 0; iG < td.m_vGroups.size(); iG++)
	{
		if (!HandleValidatedTx(td.m_vGroups[iG].m_Data, bic))
		{
			// undo partial changes
			bic.m_Fwd = false;
			while (iG--)
			{
				if (!HandleValidatedTx(td.m_vGroups[iG].m_Data, bic))
					OnCorrupted(); // although should not happen anyway
			}

			LOG_WARNING() << "Treasury invalid";
			return false;
		}
	}

	Serializer ser;
	TxoID id0 = 0;

	for (size_t iG = 0; iG < td.m_vGroups.size(); iG++)
	{
		for (size_t i = 0; i < td.m_vGroups[iG].m_Data.m_vOutputs.size(); i++, id0++)
		{
			ser.reset();
			ser & *td.m_vGroups[iG].m_Data.m_vOutputs[i];

			SerializeBuffer sb = ser.buffer();
			m_DB.TxoAdd(id0, Blob(sb.first, static_cast<uint32_t>(sb.second)));
		}
	}

	return true;
}

std::ostream& operator << (std::ostream& s, const LogSid& sid)
{
	Block::SystemState::ID id;
	id.m_Height = sid.m_Sid.m_Height;
	sid.m_DB.get_StateHash(sid.m_Sid.m_Row, id.m_Hash);

	s << id;
	return s;
}

struct NodeProcessor::KrnFlyMmr
	:public Merkle::FlyMmr
{
	const TxVectors::Eternal& m_Txve;

	KrnFlyMmr(const TxVectors::Eternal& txve)
		:m_Txve(txve)
	{
		m_Count = txve.m_vKernels.size();
	}

	virtual void LoadElement(Merkle::Hash& hv, uint64_t n) const override {
		assert(n < m_Count);
		hv = m_Txve.m_vKernels[n]->m_Internal.m_ID;
	}
};

bool NodeProcessor::HandleBlock(const NodeDB::StateID& sid, const Block::SystemState::Full& s, MultiblockContext& mbc)
{
	ByteBuffer bbP, bbE;
	m_DB.GetStateBlock(sid.m_Row, &bbP, &bbE, nullptr);

	MultiblockContext::MyTask::SharedBlock::Ptr pShared = std::make_shared<MultiblockContext::MyTask::SharedBlock>(mbc);
	Block::Body& block = pShared->m_Body;

	try {
		Deserializer der;
		der.reset(bbP);
		der & Cast::Down<Block::BodyBase>(block);
		der & Cast::Down<TxVectors::Perishable>(block);

		der.reset(bbE);
		der & Cast::Down<TxVectors::Eternal>(block);
	}
	catch (const std::exception&) {
		LOG_WARNING() << LogSid(m_DB, sid) << " Block deserialization failed";
		return false;
	}

	bool bFirstTime = (m_DB.get_StateTxos(sid.m_Row) == MaxHeight);
	if (bFirstTime)
	{
		pShared->m_Size = bbP.size() + bbE.size();
		pShared->m_Ctx.m_Height = sid.m_Height;

		PeerID pid;
		if (!m_DB.get_Peer(sid.m_Row, pid))
			pid = Zero;

		mbc.OnBlock(pid, pShared);

		Difficulty::Raw wrk = m_Cursor.m_Full.m_ChainWork + s.m_PoW.m_Difficulty;

		if (wrk != s.m_ChainWork)
		{
			LOG_WARNING() << LogSid(m_DB, sid) << " Chainwork expected=" << wrk <<", actual=" << s.m_ChainWork;
			return false;
		}

		if (m_Cursor.m_DifficultyNext.m_Packed != s.m_PoW.m_Difficulty.m_Packed)
		{
			LOG_WARNING() << LogSid(m_DB, sid) << " Difficulty expected=" << m_Cursor.m_DifficultyNext << ", actual=" << s.m_PoW.m_Difficulty;
			return false;
		}

		if (s.m_TimeStamp <= get_MovingMedian())
		{
			LOG_WARNING() << LogSid(m_DB, sid) << " Timestamp inconsistent wrt median";
			return false;
		}

		KrnFlyMmr fmmr(block);
		Merkle::Hash hv;
		fmmr.get_Hash(hv);

		if (s.m_Kernels != hv)
		{
			LOG_WARNING() << LogSid(m_DB, sid) << " Kernel commitment mismatch";
			return false;
		}
	}

	TxoID id0 = m_Extra.m_Txos;

	BlockInterpretCtx bic(sid.m_Height, true);
	bic.SetAssetHi(*this);
	if (!bFirstTime)
		bic.m_AlreadyValidated = true;

	bbP.clear();
	bic.m_pRollback = &bbP;

	bic.m_StoreShieldedOutput = true;

	bool bOk = HandleValidatedBlock(block, bic);
	if (!bOk)
	{
		assert(bFirstTime);
		assert(m_Extra.m_Txos == id0);
		LOG_WARNING() << LogSid(m_DB, sid) << " invalid in its context";
	}
	else
	{
		assert(m_Extra.m_Txos > id0);
	}

	if (bFirstTime && bOk)
	{
		if (sid.m_Height >= m_SyncData.m_TxoLo)
		{
			// check the validity of state description.
			Merkle::Hash hvDef;
			Evaluator ev(*this);
			ev.m_Height++;
			ev.get_Definition(hvDef);

			if (s.m_Definition != hvDef)
			{
				LOG_WARNING() << LogSid(m_DB, sid) << " Header Definition mismatch";
				bOk = false;
			}
		}

		if (sid.m_Height <= m_SyncData.m_TxoLo)
		{
			// make sure no spent txos above the requested h0
			for (size_t i = 0; i < block.m_vInputs.size(); i++)
			{
				if (block.m_vInputs[i]->m_Internal.m_ID >= mbc.m_id0)
				{
					LOG_WARNING() << LogSid(m_DB, sid) << " Invalid input in sparse block";
					bOk = false;
					break;
				}
			}
		}

		if (!bOk)
		{
			bic.m_Fwd = false;
			BEAM_VERIFY(HandleValidatedBlock(block, bic));
		}
	}

	if (bOk)
	{
		ECC::Scalar offsAcc = block.m_Offset;

		if (sid.m_Height > Rules::HeightGenesis)
		{
			uint64_t row = sid.m_Row;
			if (!m_DB.get_Prev(row))
				OnCorrupted();

			AdjustOffset(offsAcc, row, true);
		}

		Blob blobExtra(offsAcc.m_Value);
		Blob blobRB(bbP);
		m_DB.set_StateTxosAndExtra(sid.m_Row, &m_Extra.m_Txos, &blobExtra, &blobRB);

		std::vector<NodeDB::StateInput> v;
		v.reserve(block.m_vInputs.size());

		for (size_t i = 0; i < block.m_vInputs.size(); i++)
		{
			const Input& x = *block.m_vInputs[i];
			m_DB.TxoSetSpent(x.m_Internal.m_ID, sid.m_Height);
			v.emplace_back().Set(x.m_Internal.m_ID, x.m_Commitment);
		}

		if (!v.empty())
			m_DB.set_StateInputs(sid.m_Row, &v.front(), v.size());

		// recognize all
		for (size_t i = 0; i < block.m_vInputs.size(); i++)
			Recognize(*block.m_vInputs[i], sid.m_Height);

		Key::IPKdf* pKey = get_ViewerKey();
		if (pKey)
		{
			for (size_t i = 0; i < block.m_vOutputs.size(); i++)
				Recognize(*block.m_vOutputs[i], sid.m_Height, *pKey);
		}

		if (pKey || get_ViewerShieldedKey())
		{
			KrnWalkerRecognize wlkKrn(*this);
			wlkKrn.m_Height = sid.m_Height;

			TxoID nOuts = m_Extra.m_ShieldedOutputs;
			m_Extra.m_ShieldedOutputs -= bic.m_ShieldedOuts;

			wlkKrn.Process(block.m_vKernels);
			assert(m_Extra.m_ShieldedOutputs == nOuts);
			nOuts; // supporess unused var warning in release
		}

		Serializer ser;
		bbP.clear();
		ser.swap_buf(bbP);

		for (size_t i = 0; i < block.m_vOutputs.size(); i++)
		{
			const Output& x = *block.m_vOutputs[i];

			ser.reset();
			ser & x;

			SerializeBuffer sb = ser.buffer();
			m_DB.TxoAdd(id0++, Blob(sb.first, static_cast<uint32_t>(sb.second)));
		}

		m_RecentStates.Push(sid.m_Row, s);
	}

	return bOk;
}

void NodeProcessor::AdjustOffset(ECC::Scalar& offs, uint64_t rowid, bool bAdd)
{
	ECC::Scalar offsPrev;
	if (!m_DB.get_StateExtra(rowid, offsPrev))
		OnCorrupted();

	ECC::Scalar::Native s(offsPrev);
	if (!bAdd)
		s = -s;

	s += offs;
	offs = s;
}

template <typename TKey, typename TEvt>
bool NodeProcessor::FindEvent(const TKey& key, TEvt& evt)
{
	NodeDB::WalkerEvent wlk;
	m_DB.FindEvents(wlk, Blob(&key, sizeof(key)));

	Deserializer der;
	while (true)
	{
		if (!wlk.MoveNext())
			return false;

		proto::Event::Type::Enum eType;
		der.reset(wlk.m_Body.p, wlk.m_Body.n);
		der & eType;

		if (TEvt::s_Type == eType)
			break;
	}

	der & evt;

	return true;
}

template <typename TEvt>
void NodeProcessor::AddEventInternal(Height h, const TEvt& evt, const Blob& key)
{
	Serializer ser;
	ser & TEvt::s_Type;
	ser & evt;

	m_DB.InsertEvent(h, Blob(ser.buffer().first, static_cast<uint32_t>(ser.buffer().second)), key);
	OnEvent(h, evt);
}

template <typename TEvt, typename TKey>
void NodeProcessor::AddEvent(Height h, const TEvt& evt, const TKey& key)
{
	AddEventInternal(h, evt, Blob(&key, sizeof(key)));
}

template <typename TEvt>
void NodeProcessor::AddEvent(Height h, const TEvt& evt)
{
	AddEventInternal(h, evt, Blob(nullptr, 0));
}

void NodeProcessor::Recognize(const Input& x, Height h)
{
	const EventKey::Utxo& key = x.m_Commitment;
	proto::Event::Utxo evt;

	if (!FindEvent(key, evt))
		return;

	assert(x.m_Internal.m_Maturity); // must've already been validated
	evt.m_Maturity = x.m_Internal.m_Maturity; // in case of duplicated utxo this is necessary

	evt.m_Flags &= ~proto::Event::Flags::Add;

	AddEvent(h, evt);
}

void NodeProcessor::Recognize(const TxKernelShieldedInput& x, Height h)
{
	EventKey::Shielded key = x.m_SpendProof.m_SpendPk;
	key.m_Y |= EventKey::s_FlagShielded;

	proto::Event::Shielded evt;
	if (!FindEvent(key, evt))
		return;

	evt.m_Flags &= ~proto::Event::Flags::Add;

	AddEvent(h, evt);
}

bool NodeProcessor::KrnWalkerShielded::OnKrn(const TxKernel& krn)
{
	switch (krn.get_Subtype())
	{
	case TxKernel::Subtype::ShieldedInput:
		return OnKrnEx(Cast::Up<TxKernelShieldedInput>(krn));
	case TxKernel::Subtype::ShieldedOutput:
		return OnKrnEx(Cast::Up<TxKernelShieldedOutput>(krn));

	default:
		break; // suppress warning
	}

	return true;
}

bool NodeProcessor::KrnWalkerRecognize::OnKrn(const TxKernel& krn)
{
	switch (krn.get_Subtype())
	{
	case TxKernel::Subtype::ShieldedInput:
		m_Proc.Recognize(Cast::Up<TxKernelShieldedInput>(krn), m_Height);
		break;

	case TxKernel::Subtype::ShieldedOutput:
		m_Proc.Recognize(Cast::Up<TxKernelShieldedOutput>(krn), m_Height, m_Proc.get_ViewerShieldedKey());
		break;

	case TxKernel::Subtype::AssetCreate:
		m_Proc.Recognize(Cast::Up<TxKernelAssetCreate>(krn), m_Height, m_Proc.get_ViewerKey());
		break;

	case TxKernel::Subtype::AssetDestroy:
		m_Proc.Recognize(Cast::Up<TxKernelAssetDestroy>(krn), m_Height);
		break;

	case TxKernel::Subtype::AssetEmit:
		m_Proc.Recognize(Cast::Up<TxKernelAssetEmit>(krn), m_Height);
		break;

	default:
		break; // suppress warning
	}

	return true;
}

void NodeProcessor::Recognize(const TxKernelShieldedOutput& v, Height h, const ShieldedTxo::Viewer* pKeyShielded)
{
	TxoID nID = m_Extra.m_ShieldedOutputs++;

	if (!pKeyShielded)
		return;

	const ShieldedTxo& txo = v.m_Txo;

	ShieldedTxo::Data::SerialParams sp;
	if (!sp.Recover(txo.m_Serial, *pKeyShielded))
		return;

	ECC::Oracle oracle;
	oracle << v.m_Msg;

	ShieldedTxo::Data::OutputParams op;
	if (!op.Recover(txo, sp.m_SharedSecret, oracle))
		return;

	proto::Event::Shielded evt;
	evt.m_ID = nID;
	evt.m_Value = op.m_Value;
	evt.m_AssetID = op.m_AssetID;
	evt.m_User = op.m_User;
	evt.m_kSerG = sp.m_pK[0];
	evt.m_Flags = proto::Event::Flags::Add;
	if (sp.m_IsCreatedByViewer)
		evt.m_Flags |= proto::Event::Flags::CreatedByViewer;

	EventKey::Shielded key = sp.m_SpendPk;
	key.m_Y |= EventKey::s_FlagShielded;

	AddEvent(h, evt, key);
}

void NodeProcessor::Recognize(const Output& x, Height h, Key::IPKdf& keyViewer)
{
	CoinID cid;
	if (!x.Recover(h, keyViewer, cid))
		return;

	// filter-out dummies
	if (IsDummy(cid))
	{
		OnDummy(cid, h);
		return;
	}

	// bingo!
	proto::Event::Utxo evt;
	evt.m_Flags = proto::Event::Flags::Add;
	evt.m_Cid = cid;
	evt.m_Commitment = x.m_Commitment;
	evt.m_Maturity = x.get_MinMaturity(h);

	const EventKey::Utxo& key = x.m_Commitment;
	AddEvent(h, evt, key);
}

void NodeProcessor::Recognize(const TxKernelAssetCreate& v, Height h, Key::IPKdf* pOwner)
{
	if (!pOwner)
		return;

	EventKey::AssetCtl key;
	v.m_MetaData.get_Owner(key, *pOwner);
	if (key != v.m_Owner)
		return;

	// recognized!
	proto::Event::AssetCtl evt;

	evt.m_EmissionChange = 0; // no change upon creation
	evt.m_Flags = proto::Event::Flags::Add;

	TemporarySwap<ByteBuffer> ts(Cast::NotConst(v).m_MetaData.m_Value, evt.m_Metadata.m_Value);

	AddEvent(h, evt, key);
}

void NodeProcessor::Recognize(const TxKernelAssetEmit& v, Height h)
{
	proto::Event::AssetCtl evt;
	if (!FindEvent(v.m_Owner, evt))
		return;

	evt.m_Flags = 0;
	evt.m_EmissionChange = v.m_Value;
	AddEvent(h, evt);
}

void NodeProcessor::Recognize(const TxKernelAssetDestroy& v, Height h)
{
	proto::Event::AssetCtl evt;
	if (!FindEvent(v.m_Owner, evt))
		return;

	evt.m_Flags = proto::Event::Flags::Delete;
	AddEvent(h, evt);
}

void NodeProcessor::RescanOwnedTxos()
{
	m_DB.DeleteEventsFrom(Rules::HeightGenesis - 1);

	struct TxoRecover
		:public ITxoRecover
	{
		NodeProcessor& m_This;
		uint32_t m_Total = 0;
		uint32_t m_Unspent = 0;

		TxoRecover(Key::IPKdf& key, NodeProcessor& x)
			:ITxoRecover(key)
			,m_This(x)
		{
		}

		virtual bool OnTxo(const NodeDB::WalkerTxo& wlk, Height hCreate, Output& outp, const CoinID& cid) override
		{
			if (IsDummy(cid))
			{
				m_This.OnDummy(cid, hCreate);
				return true;
			}

			proto::Event::Utxo evt;
			evt.m_Flags = proto::Event::Flags::Add;
			evt.m_Cid = cid;
			evt.m_Commitment = outp.m_Commitment;
			evt.m_Maturity = outp.get_MinMaturity(hCreate);

			const EventKey::Utxo& key = outp.m_Commitment;
			m_This.AddEvent(hCreate, evt, key);

			m_Total++;

			if (MaxHeight == wlk.m_SpendHeight)
				m_Unspent++;
			else
			{
				evt.m_Flags = 0;
				m_This.AddEvent(wlk.m_SpendHeight, evt);
			}

			return true;
		}
	};

	Key::IPKdf* pKey = get_ViewerKey();
	if (pKey)
	{
		LOG_INFO() << "Rescanning owned Txos...";

		TxoRecover wlk(*pKey, *this);
		EnumTxos(wlk);

		LOG_INFO() << "Recovered " << wlk.m_Unspent << "/" << wlk.m_Total << " unspent/total Txos";
	}
	else
	{
		LOG_INFO() << "Owned Txos reset";
	}

	if (pKey || get_ViewerShieldedKey())
	{
		LOG_INFO() << "Rescanning shielded Txos...";

		// shielded items
		Height h0 = Rules::get().pForks[2].m_Height;
		if (m_Cursor.m_Sid.m_Height >= h0)
		{
			TxoID nOuts = m_Extra.m_ShieldedOutputs;
			m_Extra.m_ShieldedOutputs = 0;

			KrnWalkerRecognize wlkKrn(*this);
			EnumKernels(wlkKrn, HeightRange(h0, m_Cursor.m_Sid.m_Height));

			assert(m_Extra.m_ShieldedOutputs == nOuts);
			nOuts; // supporess unused var warning in release
		}

		LOG_INFO() << "Shielded scan complete";
	}
}

bool NodeProcessor::IsDummy(const CoinID&  cid)
{
	return
		!cid.m_Value &&
		!cid.m_AssetID &&
		(Key::Type::Decoy == cid.m_Type);
}

Height NodeProcessor::FindVisibleKernel(const Merkle::Hash& id, const BlockInterpretCtx& bic)
{
	Height h = m_DB.FindKernel(id);
	if (h >= Rules::HeightGenesis)
	{
		assert(h <= bic.m_Height);

		const Rules& r = Rules::get();
		if ((bic.m_Height >= r.pForks[2].m_Height) && (bic.m_Height - h > r.MaxKernelValidityDH))
			return 0; // Starting from Fork2 - visibility horizon is limited
	}

	return h;
}


bool NodeProcessor::HandleKernel(const TxKernelStd& krn, BlockInterpretCtx& bic)
{
	if (bic.m_Fwd && krn.m_pRelativeLock && !bic.m_AlreadyValidated)
	{
		const TxKernelStd::RelativeLock& x = *krn.m_pRelativeLock;

		Height h0 = FindVisibleKernel(x.m_ID, bic);
		if (h0 < Rules::HeightGenesis)
			return false;

		HeightAdd(h0, x.m_LockHeight);
		if (h0 > bic.m_Height)
			return false;
	}

	return true;
}

void NodeProcessor::InternalAssetAdd(Asset::Full& ai)
{
	ai.m_Value = Zero;
	m_DB.AssetAdd(ai);
	assert(ai.m_ID); // it's 1-based

	if (m_Mmr.m_Assets.m_Count < ai.m_ID)
		m_Mmr.m_Assets.ResizeTo(ai.m_ID);

	Merkle::Hash hv;
	ai.get_Hash(hv);
	m_Mmr.m_Assets.Replace(ai.m_ID - 1, hv);
}

void NodeProcessor::InternalAssetDel(Asset::ID nAssetID)
{
	Asset::ID nCount = m_DB.AssetDelete(nAssetID);

	assert(nCount <= m_Mmr.m_Assets.m_Count);
	if (nCount < m_Mmr.m_Assets.m_Count)
		m_Mmr.m_Assets.ResizeTo(nCount);
	else
	{
		assert(nAssetID < nCount);
		m_Mmr.m_Assets.Replace(nAssetID - 1, Zero);
	}
}

bool NodeProcessor::HandleKernel(const TxKernelAssetCreate& krn, BlockInterpretCtx& bic)
{
	if (!bic.m_AlreadyValidated)
	{
		bic.EnsureAssetsUsed(m_DB);

		if (bic.m_Fwd)
		{
			if (m_DB.AssetFindByOwner(krn.m_Owner))
				return false;

			if (bic.m_AssetsUsed >= Asset::s_MaxCount)
				return false;

			bic.m_AssetsUsed++;
		}
		else
		{
			assert(bic.m_AssetsUsed);
			bic.m_AssetsUsed--;
		}
	}

	if (!bic.m_UpdateMmrs)
		return true;

	assert(!bic.m_ValidateOnly);

	if (bic.m_Fwd)
	{
		Asset::Full ai;
		ai.m_ID = 0; // auto
		ai.m_Owner = krn.m_Owner;
		ai.m_LockHeight = bic.m_Height;

		ai.m_Metadata.m_Hash = krn.m_MetaData.m_Hash;
		TemporarySwap<ByteBuffer> ts(Cast::NotConst(krn).m_MetaData.m_Value, ai.m_Metadata.m_Value);

		InternalAssetAdd(ai);

		BlockInterpretCtx::Ser ser(bic);
		ser & ai.m_ID;
	}
	else
	{
		BlockInterpretCtx::Der der(bic);

		Asset::ID nVal;
		der & nVal;

		InternalAssetDel(nVal);
	}

	return true;
}

bool NodeProcessor::HandleKernel(const TxKernelAssetDestroy& krn, BlockInterpretCtx& bic)
{
	if (!bic.m_AlreadyValidated)
		bic.EnsureAssetsUsed(m_DB);

	if (bic.m_Fwd)
	{
		Asset::Full ai;
		ai.m_ID = krn.m_AssetID;
		if (!m_DB.AssetGetSafe(ai))
			return false;

		if (!bic.m_AlreadyValidated)
		{
			if (ai.m_Owner != krn.m_Owner)
				return false;

			if (ai.m_Value != Zero)
				return false;

			if (ai.m_LockHeight + Rules::get().CA.LockPeriod > bic.m_Height)
				return false;

			assert(bic.m_AssetsUsed);
			bic.m_AssetsUsed--;
		}

		if (bic.m_UpdateMmrs)
		{
			// looks good
			InternalAssetDel(krn.m_AssetID);

			BlockInterpretCtx::Ser ser(bic);
			ser
				& ai.m_Metadata
				& ai.m_LockHeight;
		}
	}
	else
	{
		if (bic.m_UpdateMmrs)
		{
			Asset::Full ai;
			ai.m_ID = krn.m_AssetID;
			ai.m_Owner = krn.m_Owner;

			BlockInterpretCtx::Der der(bic);
			der
				& ai.m_Metadata
				& ai.m_LockHeight;

			InternalAssetAdd(ai);

			if (ai.m_ID != krn.m_AssetID)
				OnCorrupted();
		}

		if (!bic.m_AlreadyValidated)
		{
			bic.m_AssetsUsed++;
			assert(bic.m_AssetsUsed <= Asset::s_MaxCount);
		}
	}

	return true;
}



bool NodeProcessor::HandleKernel(const TxKernelAssetEmit& krn, BlockInterpretCtx& bic)
{
	if (!bic.m_Fwd && !bic.m_UpdateMmrs)
		return true;

	Asset::Full ai;
	ai.m_ID = krn.m_AssetID;
	if (!m_DB.AssetGetSafe(ai))
		return false;
	if (ai.m_Owner != krn.m_Owner)
		return false; // as well

	AmountSigned val = krn.m_Value;
	bool bAdd = (val >= 0);
	if (!bAdd)
	{
		val = -val;
		if (val < 0)
			// can happen if val is 0x800....0, such a number can't be negated on its own. Ban this case
			return false;
	}

	AmountBig::Type valBig = (Amount) val;
	if (!bic.m_Fwd)
		bAdd = !bAdd;

	bool bWasZero = (ai.m_Value == Zero);

	if (bAdd)
	{
		ai.m_Value += valBig;
		if (ai.m_Value < valBig)
			return false; // overflow (?!)
	}
	else
	{
		if (ai.m_Value < valBig)
			return false; // not enough to burn

		valBig.Negate();
		ai.m_Value += valBig;
	}

	if (bic.m_UpdateMmrs)
	{
		bool bZero = (ai.m_Value == Zero);
		if (bZero != bWasZero)
		{
			if (bic.m_Fwd)
			{
				BlockInterpretCtx::Ser ser(bic);
				ser & ai.m_LockHeight;

				ai.m_LockHeight = bic.m_Height;
			}
			else
			{
				BlockInterpretCtx::Der der(bic);
				der & ai.m_LockHeight;
			}
		}

		m_DB.AssetSetValue(ai.m_ID, ai.m_Value, ai.m_LockHeight);

		Merkle::Hash hv;
		ai.get_Hash(hv);

		m_Mmr.m_Assets.Replace(ai.m_ID - 1, hv);
	}

	return true;
}

bool NodeProcessor::HandleKernel(const TxKernelShieldedOutput& krn, BlockInterpretCtx& bic)
{
	const ECC::Point& key = krn.m_Txo.m_Serial.m_SerialPub;
	Blob blobKey(&key, sizeof(key));

	if (bic.m_Fwd)
	{
		if (bic.m_ShieldedOuts >= Rules::get().Shielded.MaxOuts)
		{
			bic.m_LimitExceeded = true;
			return false;
		}

		if (!bic.ValidateAssetRange(krn.m_Txo.m_pAsset))
			return false;

		if (bic.m_ValidateOnly)
		{
			if (!ValidateUniqueNoDup(bic, blobKey))
				return false;
		}
		else
		{
			ShieldedOutpPacked sop;
			sop.m_Height = bic.m_Height;
			sop.m_MmrIndex = m_Mmr.m_Shielded.m_Count;
			sop.m_TxoID = m_Extra.m_ShieldedOutputs;
			sop.m_Commitment = krn.m_Txo.m_Commitment;

			Blob blobVal(&sop, sizeof(sop));

			if (!m_DB.UniqueInsertSafe(blobKey, &blobVal))
				return false;

			if (bic.m_StoreShieldedOutput)
			{
				ECC::Point::Native pt, pt2;
				pt.Import(krn.m_Txo.m_Commitment); // don't care if Import fails (kernels are not necessarily tested at this stage)
				pt2.Import(krn.m_Txo.m_Serial.m_SerialPub);
				pt += pt2;

				ECC::Point::Storage pt_s;
				pt.Export(pt_s);

				m_DB.ShieldedResize(m_Extra.m_ShieldedOutputs + 1, m_Extra.m_ShieldedOutputs);
				// Append to cmList
				m_DB.ShieldedWrite(m_Extra.m_ShieldedOutputs, &pt_s, 1);
			}

			if (bic.m_UpdateMmrs)
			{
				ShieldedTxo::DescriptionOutp d;
				d.m_SerialPub = krn.m_Txo.m_Serial.m_SerialPub;
				d.m_Commitment = krn.m_Txo.m_Commitment;
				d.m_ID = m_Extra.m_ShieldedOutputs;
				d.m_Height = bic.m_Height;

				Merkle::Hash hv;
				d.get_Hash(hv);
				m_Mmr.m_Shielded.Append(hv);
			}

			m_Extra.m_ShieldedOutputs++;
		}

		bic.m_ShieldedOuts++; // ok

	}
	else
	{
		assert(!bic.m_ValidateOnly);

		m_DB.UniqueDeleteStrict(blobKey);

		if (bic.m_UpdateMmrs)
			m_Mmr.m_Shielded.ShrinkTo(m_Mmr.m_Shielded.m_Count - 1);

		if (bic.m_StoreShieldedOutput)
			m_DB.ShieldedResize(m_Extra.m_ShieldedOutputs - 1, m_Extra.m_ShieldedOutputs);

		assert(bic.m_ShieldedOuts);
		bic.m_ShieldedOuts--;

		assert(m_Extra.m_ShieldedOutputs);
		m_Extra.m_ShieldedOutputs--;
	}

	if (bic.m_StoreShieldedOutput)
		m_DB.ParamIntSet(NodeDB::ParamID::ShieldedOutputs, m_Extra.m_ShieldedOutputs);

	return true;
}

bool NodeProcessor::HandleKernel(const TxKernelShieldedInput& krn, BlockInterpretCtx& bic)
{
	ECC::Point key = krn.m_SpendProof.m_SpendPk;
	key.m_Y |= 2;
	Blob blobKey(&key, sizeof(key));

	if (bic.m_Fwd)
	{
		if (!bic.m_AlreadyValidated)
		{
			if (!bic.ValidateAssetRange(krn.m_pAsset))
				return false;

			if (bic.m_ShieldedIns >= Rules::get().Shielded.MaxIns)
			{
				bic.m_LimitExceeded = true;
				return false;
			}

			if (!IsShieldedInPool(krn))
				return false; // references invalid pool window
		}

		if (bic.m_ValidateOnly)
		{
			if (!ValidateUniqueNoDup(bic, blobKey))
				return false;
		}
		else
		{
			ShieldedInpPacked sip;
			sip.m_Height = bic.m_Height;
			sip.m_MmrIndex = m_Mmr.m_Shielded.m_Count;

			Blob blobVal(&sip, sizeof(sip));

			if (!m_DB.UniqueInsertSafe(blobKey, &blobVal))
				return false;

			if (bic.m_UpdateMmrs)
			{
				ShieldedTxo::DescriptionInp d;
				d.m_SpendPk = krn.m_SpendProof.m_SpendPk;
				d.m_Height = bic.m_Height;

				Merkle::Hash hv;
				d.get_Hash(hv);
				m_Mmr.m_Shielded.Append(hv);
			}
		}

		bic.m_ShieldedIns++; // ok

	}
	else
	{
		assert(!bic.m_ValidateOnly);

		m_DB.UniqueDeleteStrict(blobKey);

		if (bic.m_UpdateMmrs)
			m_Mmr.m_Shielded.ShrinkTo(m_Mmr.m_Shielded.m_Count - 1);

		assert(bic.m_ShieldedIns);
		bic.m_ShieldedIns--;
	}

	if (bic.m_StoreShieldedOutput)
	{
		assert(bic.m_UpdateMmrs); // otherwise the following formula will be wrong

		TxoID nShieldedInputs = m_Mmr.m_Shielded.m_Count - m_Extra.m_ShieldedOutputs;
		m_DB.ParamIntSet(NodeDB::ParamID::ShieldedInputs, nShieldedInputs);
	}


	return true;
}

template <typename T>
bool NodeProcessor::HandleElementVecFwd(const T& vec, BlockInterpretCtx& bic, size_t& n)
{
	assert(bic.m_Fwd);

	for (; n < vec.size(); n++)
		if (!HandleBlockElement(*vec[n], bic))
			return false;

	return true;
}

template <typename T>
void NodeProcessor::HandleElementVecBwd(const T& vec, BlockInterpretCtx& bic, size_t n)
{
	assert(!bic.m_Fwd);

	while (n--)
		if (!HandleBlockElement(*vec[n], bic))
			OnCorrupted();
}

bool NodeProcessor::HandleValidatedTx(const TxVectors::Full& txv, BlockInterpretCtx& bic)
{
	size_t pN[3];

	bool bOk = true;
	if (bic.m_Fwd)
	{
		ZeroObject(pN);
		bOk =
			HandleElementVecFwd(txv.m_vInputs, bic, pN[0]) &&
			HandleElementVecFwd(txv.m_vOutputs, bic, pN[1]) &&
			HandleElementVecFwd(txv.m_vKernels, bic, pN[2]);

		if (bOk)
			return true;

		bic.m_Fwd = false; // rollback partial changes
	}
	else
	{
		// rollback all
		pN[0] = txv.m_vInputs.size();
		pN[1] = txv.m_vOutputs.size();
		pN[2] = txv.m_vKernels.size();
	}

	HandleElementVecBwd(txv.m_vKernels, bic, pN[2]);
	HandleElementVecBwd(txv.m_vOutputs, bic, pN[1]);
	HandleElementVecBwd(txv.m_vInputs, bic, pN[0]);

	if (!bOk)
		bic.m_Fwd = true; // restore it to prevent confuse

	return bOk;
}

bool NodeProcessor::HandleValidatedBlock(const Block::Body& block, BlockInterpretCtx& bic)
{
	// make sure we adjust txo count, to prevent the same Txos for consecutive blocks after cut-through
	if (!bic.m_Fwd)
	{
		assert(m_Extra.m_Txos);
		m_Extra.m_Txos--;
	}

	if (!HandleValidatedTx(block, bic))
		return false;

	// currently there's no extra info in the block that's needed

	if (bic.m_Fwd)
		m_Extra.m_Txos++;

	return true;
}

bool NodeProcessor::HandleBlockElement(const Input& v, BlockInterpretCtx& bic)
{
	UtxoTree::Cursor cu;
	UtxoTree::MyLeaf* p;
	UtxoTree::Key::Data d;
	d.m_Commitment = v.m_Commitment;

	if (bic.m_Fwd)
	{
		struct Traveler :public UtxoTree::ITraveler {
			virtual bool OnLeaf(const RadixTree::Leaf& x) override {
				return false; // stop iteration
			}
		} t;


		UtxoTree::Key kMin, kMax;

		d.m_Maturity = Rules::HeightGenesis - 1;
		kMin = d;
		d.m_Maturity = bic.m_Height - 1;
		kMax = d;

		t.m_pCu = &cu;
		t.m_pBound[0] = kMin.V.m_pData;
		t.m_pBound[1] = kMax.V.m_pData;

		if (m_Utxos.Traverse(t))
			return false;

		p = &Cast::Up<UtxoTree::MyLeaf>(cu.get_Leaf());

		d = p->m_Key;
		assert(d.m_Commitment == v.m_Commitment);
		assert(d.m_Maturity < bic.m_Height);

		TxoID nID = p->m_ID;

		if (!p->IsExt())
			m_Utxos.Delete(cu);
		else
		{
			nID = m_Utxos.PopID(*p);
			cu.InvalidateElement();
			m_Utxos.OnDirty();
		}

		Cast::NotConst(v).m_Internal.m_Maturity = d.m_Maturity;
		Cast::NotConst(v).m_Internal.m_ID = nID;

	} else
	{
		d.m_Maturity = v.m_Internal.m_Maturity;

		bool bCreate = true;
		UtxoTree::Key key;
		key = d;

		m_Utxos.EnsureReserve();

		p = m_Utxos.Find(cu, key, bCreate);

		if (bCreate)
			p->m_ID = v.m_Internal.m_ID;
		else
		{
			m_Utxos.PushID(v.m_Internal.m_ID, *p);
			cu.InvalidateElement();
			m_Utxos.OnDirty();
		}
	}

	return true;
}

bool NodeProcessor::HandleBlockElement(const Output& v, BlockInterpretCtx& bic)
{
	UtxoTree::Key::Data d;
	d.m_Commitment = v.m_Commitment;
	d.m_Maturity = v.get_MinMaturity(bic.m_Height);

	UtxoTree::Key key;
	key = d;

	m_Utxos.EnsureReserve();

	UtxoTree::Cursor cu;
	bool bCreate = true;
	UtxoTree::MyLeaf* p = m_Utxos.Find(cu, key, bCreate);

	cu.InvalidateElement();
	m_Utxos.OnDirty();

	if (bic.m_Fwd)
	{
		if (!bic.ValidateAssetRange(v.m_pAsset))
			return false;

		TxoID nID = m_Extra.m_Txos;

		if (bCreate)
			p->m_ID = nID;
		else
		{
			// protect again overflow attacks, though it's highly unlikely (Input::Count is currently limited to 32 bits, it'd take millions of blocks)
			Input::Count nCountInc = p->get_Count() + 1;
			if (!nCountInc)
				return false;

			m_Utxos.PushID(nID, *p);
		}

		m_Extra.m_Txos++;

	} else
	{
		assert(m_Extra.m_Txos);
		m_Extra.m_Txos--;

		if (!p->IsExt())
			m_Utxos.Delete(cu);
		else
			m_Utxos.PopID(*p);
	}

	return true;
}

bool NodeProcessor::HandleBlockElement(const TxKernel& v, BlockInterpretCtx& bic)
{
	const Rules& r = Rules::get();
	if (bic.m_Fwd && (bic.m_Height >= r.pForks[2].m_Height) && !bic.m_AlreadyValidated)
	{
		Height hPrev = FindVisibleKernel(v.m_Internal.m_ID, bic);
		if (hPrev >= Rules::HeightGenesis)
			return false; // duplicated

		if (bic.m_ValidateOnly)
		{
			assert(bic.m_pDupIDs);
			Blob key(v.m_Internal.m_ID);

			if (bic.m_pDupIDs->end() != bic.m_pDupIDs->find(key))
				return false; // duplicated within the same tx

			bic.m_pDupIDs->insert(key);
			
		}
	}

	bool bSaveID = ((bic.m_Height >= Rules::HeightGenesis) && bic.m_SaveKid); // for historical reasons treasury kernels are ignored
	if (bSaveID && !bic.m_Fwd)
		m_DB.DeleteKernel(v.m_Internal.m_ID, bic.m_Height);

	if (!HandleKernel(v, bic))
	{
		if (!bic.m_Fwd)
			OnCorrupted();
		return false;
	}

	if (bSaveID && bic.m_Fwd)
		m_DB.InsertKernel(v.m_Internal.m_ID, bic.m_Height);

	return true;
}

bool NodeProcessor::HandleKernel(const TxKernel& v, BlockInterpretCtx& bic)
{
	size_t n = 0;
	bool bOk = true;

	if (bic.m_Fwd)
	{
		// nested
		for (; n < v.m_vNested.size(); n++)
		{
			if (!HandleKernel(*v.m_vNested[n], bic))
			{
				bOk = false;
				break;
			}
		}
	}
	else
		n = v.m_vNested.size();

	if (bOk)
	{
		switch (v.get_Subtype())
		{
#define THE_MACRO(id, name) \
		case TxKernel::Subtype::name: \
			bOk = HandleKernel(Cast::Up<TxKernel##name>(v), bic); \
			break;

		BeamKernelsAll(THE_MACRO)
#undef THE_MACRO

		}
	}

	if (!bOk)
	{
		if (!bic.m_Fwd)
			OnCorrupted();
		bic.m_Fwd = false;
	}

	if (!bic.m_Fwd && !bic.m_ValidateOnly) // for validate-only mode no need to revert the changes
	{
		// nested
		while (n--)
			if (!HandleKernel(*v.m_vNested[n], bic))
				OnCorrupted();
	}

	if (!bOk)
		bic.m_Fwd = true; // restore it back

	return bOk;
}

bool NodeProcessor::IsShieldedInPool(const Transaction& tx)
{
	struct Walker
		:public TxKernel::IWalker
	{
		NodeProcessor* m_pThis;
		virtual bool OnKrn(const TxKernel& krn) override
		{
			if (krn.get_Subtype() != TxKernel::Subtype::ShieldedInput)
				return true;

			return m_pThis->IsShieldedInPool(Cast::Up<TxKernelShieldedInput>(krn));
		}
	} wlk;
	wlk.m_pThis = this;

	return wlk.Process(tx.m_vKernels);
}

bool NodeProcessor::IsShieldedInPool(const TxKernelShieldedInput& krn)
{
	const Rules& r = Rules::get();
	if (!r.Shielded.Enabled)
		return false;

	if (krn.m_WindowEnd > m_Extra.m_ShieldedOutputs)
		return false;

	if (!(krn.m_SpendProof.m_Cfg == r.Shielded.m_ProofMin))
	{
		if (!(krn.m_SpendProof.m_Cfg == r.Shielded.m_ProofMax))
			return false; // cfg not allowed

		if (m_Extra.m_ShieldedOutputs > krn.m_WindowEnd + r.Shielded.MaxWindowBacklog)
			return false; // large anonymity set is no more allowed, expired
	}

	return true;
}

void NodeProcessor::BlockInterpretCtx::EnsureAssetsUsed(NodeDB& db)
{
	if (m_AssetsUsed == Asset::s_MaxCount + 1)
		m_AssetsUsed = static_cast<Asset::ID>(db.ParamIntGetDef(NodeDB::ParamID::AssetsCountUsed));
}

NodeProcessor::BlockInterpretCtx::BlobSet::~BlobSet()
{
	Clear();
}

void NodeProcessor::BlockInterpretCtx::BlobSet::Clear()
{
	while (!empty())
	{
		BlobItem& x = *begin();
		erase(x);
		delete& x;
	}
}

bool NodeProcessor::BlockInterpretCtx::BlobSet::Find(const Blob& key) const
{
	struct Comparator
	{
		bool operator()(const Blob& a, const BlobItem& b) const {
			return a < b.ToBlob();
		}
		bool operator()(const BlobItem& a, const Blob& b) const {
			return a.ToBlob() < b;
		}
	};

	return end() != find(key, Comparator());
}

void NodeProcessor::BlockInterpretCtx::BlobSet::Add(const Blob& key)
{
	BlobItem* pItem = new (key.n) BlobItem;
	pItem->m_Size = key.n;
	memcpy(pItem->m_pBuf, key.p, key.n);

	insert(*pItem);
}

NodeProcessor::BlockInterpretCtx::Ser::Ser(BlockInterpretCtx& bic)
	:m_This(bic)
{
	assert(bic.m_pRollback);
	m_Pos = bic.m_pRollback->size();
	swap_buf(*bic.m_pRollback);
}

NodeProcessor::BlockInterpretCtx::Ser::~Ser()
{
	if (!std::uncaught_exceptions())
	{
		Marker mk = static_cast<uint32_t>(buffer().second - m_Pos);
		*this & mk;
	}
	swap_buf(*m_This.m_pRollback);
}

NodeProcessor::BlockInterpretCtx::Der::Der(BlockInterpretCtx& bic)
{
	assert(bic.m_pRollback);
	ByteBuffer& buf = *bic.m_pRollback; // alias

	Ser::Marker mk;
	SetBwd(buf, mk.nBytes);
	*this & mk;

	uint32_t n;
	mk.Export(n);
	SetBwd(buf, n);
}

void NodeProcessor::BlockInterpretCtx::Der::SetBwd(ByteBuffer& buf, uint32_t nPortion)
{
	if (buf.size() < nPortion)
		OnCorrupted();

	size_t nVal = buf.size() - nPortion;
	reset(&buf.front() + nVal, nPortion);

	buf.resize(nVal); // it's safe to call resize() while the buffer is being used, coz std::vector does NOT reallocate on shrink
}

bool NodeProcessor::ValidateUniqueNoDup(BlockInterpretCtx& bic, const Blob& key)
{
	assert(bic.m_pDups);
	if (bic.m_pDups->Find(key))
		return false;

	NodeDB::Recordset rs;
	if (m_DB.UniqueFind(key, rs))
		return false;

	bic.m_pDups->Add(key);

	return true;
}

void NodeProcessor::ToInputWithMaturity(Input& inp, TxoID id)
{
	// awkward and relatively used, but this is not used frequently.
	// NodeDB::StateInput doesn't contain the maturity of the spent UTXO. Hence we reconstruct it
	// We find the original UTXO height, and then decode the UTXO body, and check its additional maturity factors (coinbase, incubation)

	NodeDB::WalkerTxo wlk;
	m_DB.TxoGetValue(wlk, id);

	uint8_t pNaked[s_TxoNakedMax];
	Blob val = wlk.m_Value;
	TxoToNaked(pNaked, val);

	Deserializer der;
	der.reset(val.p, val.n);

	Output outp;
	der & outp;

	inp.m_Commitment = outp.m_Commitment;
	inp.m_Internal.m_ID = id;

	Height hCreate;
	FindHeightByTxoID(hCreate, id); // relatively heavy operation: search for the original txo height

	inp.m_Internal.m_Maturity = outp.get_MinMaturity(hCreate);
}

void NodeProcessor::RollbackTo(Height h)
{
	assert(h <= m_Cursor.m_Sid.m_Height);
	if (h == m_Cursor.m_Sid.m_Height)
		return;

	assert(h >= m_Extra.m_Fossil);

	TxoID id0 = get_TxosBefore(h + 1);

	// undo inputs
	for (NodeDB::StateID sid = m_Cursor.m_Sid; sid.m_Height > h; )
	{
		std::vector<NodeDB::StateInput> v;
		m_DB.get_StateInputs(sid.m_Row, v);

		BlockInterpretCtx bic(sid.m_Height, false);
		for (size_t i = 0; i < v.size(); i++)
		{
			TxoID id = v[i].get_ID();
			if (id >= id0)
				continue; // created and spent within this range - skip it

			Input inp;
			ToInputWithMaturity(inp, id);

			if (!HandleBlockElement(inp, bic))
				OnCorrupted();

			m_DB.TxoSetSpent(id, MaxHeight);
		}

		m_DB.set_StateInputs(sid.m_Row, nullptr, 0);

		if (!m_DB.get_Prev(sid))
			ZeroObject(sid);
	}

	// undo outputs
	struct MyWalker
		:public ITxoWalker_UnspentNaked
	{
		NodeProcessor* m_pThis;

		virtual bool OnTxo(const NodeDB::WalkerTxo& wlk, Height hCreate, Output& outp) override
		{
			BlockInterpretCtx bic(hCreate, false);
			if (!m_pThis->HandleBlockElement(outp, bic))
				OnCorrupted();
			return true;
		}
	};

	MyWalker wlk2;
	wlk2.m_pThis = this;
	EnumTxos(wlk2, HeightRange(h + 1, m_Cursor.m_Sid.m_Height));

	m_DB.TxoDelFrom(id0);
	m_DB.DeleteEventsFrom(h + 1);

	// Kernels, shielded elements, and cursor
	ByteBuffer bbE, bbR;
	TxVectors::Eternal txve;

	for (; m_Cursor.m_Sid.m_Height > h; m_DB.MoveBack(m_Cursor.m_Sid))
	{
		txve.m_vKernels.clear();
		bbE.clear();
		bbR.clear();
		m_DB.GetStateBlock(m_Cursor.m_Sid.m_Row, nullptr, &bbE, &bbR);

		Deserializer der;
		der.reset(bbE);
		der & Cast::Down<TxVectors::Eternal>(txve);

		BlockInterpretCtx bic(m_Cursor.m_Sid.m_Height, false);
		bic.m_StoreShieldedOutput = true;
		bic.m_pRollback = &bbR;
		bic.m_ShieldedIns = static_cast<uint32_t>(-1); // suppress assertion
		bic.m_ShieldedOuts = static_cast<uint32_t>(-1);
		HandleElementVecBwd(txve.m_vKernels, bic, txve.m_vKernels.size());
		assert(bbR.empty());
	}


	m_RecentStates.RollbackTo(h);

	m_Mmr.m_States.ShrinkTo(m_Mmr.m_States.H2I(m_Cursor.m_Sid.m_Height));

	m_Extra.m_Txos = id0;

	InitCursor(false);
	if (!TestDefinition())
		OnCorrupted();

	OnRolledBack();
}

NodeProcessor::DataStatus::Enum NodeProcessor::OnStateInternal(const Block::SystemState::Full& s, Block::SystemState::ID& id, bool bAlreadyChecked)
{
	s.get_ID(id);

	if (!(bAlreadyChecked || s.IsValid()))
	{
		LOG_WARNING() << id << " header invalid!";
		return DataStatus::Invalid;
	}

	Timestamp ts = getTimestamp();
	if (s.m_TimeStamp > ts)
	{
		ts = s.m_TimeStamp - ts; // dt
		if (ts > Rules::get().DA.MaxAhead_s)
		{
			LOG_WARNING() << id << " Timestamp ahead by " << ts;
			return DataStatus::Invalid;
		}
	}

	if (s.m_Height < get_LowestReturnHeight())
		return DataStatus::Unreachable;

	if (m_DB.StateFindSafe(id))
		return DataStatus::Rejected;

	return DataStatus::Accepted;
}

NodeProcessor::DataStatus::Enum NodeProcessor::OnState(const Block::SystemState::Full& s, const PeerID& peer)
{
	Block::SystemState::ID id;

	DataStatus::Enum ret = OnStateSilent(s, peer, id, false);
	if (DataStatus::Accepted == ret)
	{
		LOG_INFO() << id << " Header accepted";
	}
	
	return ret;
}

NodeProcessor::DataStatus::Enum NodeProcessor::OnStateSilent(const Block::SystemState::Full& s, const PeerID& peer, Block::SystemState::ID& id, bool bAlreadyChecked)
{
	DataStatus::Enum ret = OnStateInternal(s, id, bAlreadyChecked);
	if (DataStatus::Accepted == ret)
		m_DB.InsertState(s, peer);

	return ret;
}

NodeProcessor::DataStatus::Enum NodeProcessor::OnBlock(const Block::SystemState::ID& id, const Blob& bbP, const Blob& bbE, const PeerID& peer)
{
	NodeDB::StateID sid;
	sid.m_Row = m_DB.StateFindSafe(id);
	if (!sid.m_Row)
	{
		LOG_WARNING() << id << " Block unexpected";
		return DataStatus::Rejected;
	}

	sid.m_Height = id.m_Height;
	return OnBlock(sid, bbP, bbE, peer);
}

NodeProcessor::DataStatus::Enum NodeProcessor::OnBlock(const NodeDB::StateID& sid, const Blob& bbP, const Blob& bbE, const PeerID& peer)
{
	size_t nSize = size_t(bbP.n) + size_t(bbE.n);
	if (nSize > Rules::get().MaxBodySize)
	{
		LOG_WARNING() << LogSid(m_DB, sid) << " Block too large: " << nSize;
		return DataStatus::Invalid;
	}

	if (NodeDB::StateFlags::Functional & m_DB.GetStateFlags(sid.m_Row))
	{
		LOG_WARNING() << LogSid(m_DB, sid) << " Block already received";
		return DataStatus::Rejected;
	}

	if (sid.m_Height < get_LowestReturnHeight())
		return DataStatus::Unreachable;

	m_DB.SetStateBlock(sid.m_Row, bbP, bbE, peer);
	m_DB.SetStateFunctional(sid.m_Row);

	return DataStatus::Accepted;
}

NodeProcessor::DataStatus::Enum NodeProcessor::OnTreasury(const Blob& blob)
{
	if (Rules::get().TreasuryChecksum == Zero)
		return DataStatus::Invalid; // should be no treasury

	ECC::Hash::Value hv;
	ECC::Hash::Processor()
		<< blob
		>> hv;

	if (Rules::get().TreasuryChecksum != hv)
		return DataStatus::Invalid;

	if (IsTreasuryHandled())
		return DataStatus::Rejected;

	if (!HandleTreasury(blob))
		return DataStatus::Invalid;

	m_Extra.m_Txos++;
	m_Extra.m_TxosTreasury = m_Extra.m_Txos;
	m_DB.ParamSet(NodeDB::ParamID::Treasury, &m_Extra.m_TxosTreasury, &blob);

	LOG_INFO() << "Treasury verified";

	RescanOwnedTxos();

	OnNewState();
	TryGoUp();

	return DataStatus::Accepted;
}

bool NodeProcessor::IsRemoteTipNeeded(const Block::SystemState::Full& sTipRemote, const Block::SystemState::Full& sTipMy)
{
	int n = sTipMy.m_ChainWork.cmp(sTipRemote.m_ChainWork);
	if (n > 0)
		return false;
	if (n < 0)
		return true;

	return sTipMy != sTipRemote;
}

uint64_t NodeProcessor::FindActiveAtStrict(Height h)
{
	const RecentStates::Entry* pE = m_RecentStates.Get(h);
	if (pE)
		return pE->m_RowID;

	return m_DB.FindActiveStateStrict(h);
}

/////////////////////////////
// Block generation
Difficulty NodeProcessor::get_NextDifficulty()
{
	const Rules& r = Rules::get(); // alias

	if (!m_Cursor.m_Sid.m_Row)
		return r.DA.Difficulty0; // 1st block

	THW thw0, thw1;

	get_MovingMedianEx(m_Cursor.m_Sid.m_Height, r.DA.WindowMedian1, thw1);

	if (m_Cursor.m_Full.m_Height - Rules::HeightGenesis >= r.DA.WindowWork)
	{
		get_MovingMedianEx(m_Cursor.m_Full.m_Height - r.DA.WindowWork, r.DA.WindowMedian1, thw0);
	}
	else
	{
		get_MovingMedianEx(Rules::HeightGenesis, r.DA.WindowMedian1, thw0); // awkward to look for median, since they're immaginary. But makes sure we stick to the same median search and rounding (in case window is even).

		// how many immaginary prehistoric blocks should be offset
		uint32_t nDelta = r.DA.WindowWork - static_cast<uint32_t>(m_Cursor.m_Full.m_Height - Rules::HeightGenesis);

		thw0.first -= r.DA.Target_s * nDelta;
		thw0.second.first -= nDelta;

		Difficulty::Raw wrk, wrk2;
		r.DA.Difficulty0.Unpack(wrk);
		wrk2.AssignMul(wrk, uintBigFrom(nDelta));
		wrk2.Negate();
		thw0.second.second += wrk2;
	}

	assert(r.DA.WindowWork > r.DA.WindowMedian1); // when getting median - the target height can be shifted by some value, ensure it's smaller than the window
	// means, the height diff should always be positive
	assert(thw1.second.first > thw0.second.first);

	uint32_t dh = static_cast<uint32_t>(thw1.second.first - thw0.second.first);

	uint32_t dtTrg_s = r.DA.Target_s * dh;

	// actual dt, only making sure it's non-negative
	uint32_t dtSrc_s = (thw1.first > thw0.first) ? static_cast<uint32_t>(thw1.first - thw0.first) : 0;

	if (m_Cursor.m_Full.m_Height >= r.pForks[1].m_Height)
	{
		// Apply dampening. Recalculate dtSrc_s := dtSrc_s * M/N + dtTrg_s * (N-M)/N
		// Use 64-bit arithmetic to avoid overflow

		uint64_t nVal =
			static_cast<uint64_t>(dtSrc_s) * r.DA.Damp.M +
			static_cast<uint64_t>(dtTrg_s) * (r.DA.Damp.N - r.DA.Damp.M);

		uint32_t dt_s = static_cast<uint32_t>(nVal / r.DA.Damp.N);

		if ((dt_s > dtSrc_s) != (dt_s > dtTrg_s)) // another overflow verification. The result normally must sit between src and trg (assuming valid damp parameters, i.e. M < N).
			dtSrc_s = dt_s;
	}

	// apply "emergency" threshold
	std::setmin(dtSrc_s, dtTrg_s * 2);
	std::setmax(dtSrc_s, dtTrg_s / 2);


	Difficulty::Raw& dWrk = thw0.second.second;
	dWrk.Negate();
	dWrk += thw1.second.second;

	Difficulty res;
	res.Calculate(dWrk, dh, dtTrg_s, dtSrc_s);

	return res;
}

void NodeProcessor::get_MovingMedianEx(Height hLast, uint32_t nWindow, THW& res)
{
	std::vector<THW> v;
	v.reserve(nWindow);

	assert(hLast >= Rules::HeightGenesis);
	uint64_t rowLast = 0;

	while (v.size() < nWindow)
	{
		v.emplace_back();
		THW& thw = v.back();

		if (hLast >= Rules::HeightGenesis)
		{
			const RecentStates::Entry* pE = m_RecentStates.Get(hLast);

			Block::SystemState::Full sDb;
			if (!pE)
			{
				if (rowLast)
				{
					if (!m_DB.get_Prev(rowLast))
						OnCorrupted();
				}
				else
					rowLast = FindActiveAtStrict(hLast);

				m_DB.get_State(rowLast, sDb);
			}

			const Block::SystemState::Full& s = pE ? pE->m_State : sDb;

			thw.first = s.m_TimeStamp;
			thw.second.first = s.m_Height;
			thw.second.second = s.m_ChainWork;

			hLast--;
		}
		else
		{
			// append "prehistoric" blocks of starting difficulty and perfect timing
			const THW& thwSrc = v[v.size() - 2];

			thw.first = thwSrc.first - Rules::get().DA.Target_s;
			thw.second.first = thwSrc.second.first - 1;
			thw.second.second = thwSrc.second.second - Rules::get().DA.Difficulty0; // don't care about overflow
		}
	}

	std::sort(v.begin(), v.end()); // there's a better algorithm to find a median (or whatever order), however our array isn't too big, so it's ok.
	// In case there are multiple blocks with exactly the same Timestamp - the ambiguity is resolved w.r.t. Height.

	res = v[nWindow >> 1];
}

Timestamp NodeProcessor::get_MovingMedian()
{
	if (!m_Cursor.m_Sid.m_Row)
		return 0;

	THW thw;
	get_MovingMedianEx(m_Cursor.m_Sid.m_Height, Rules::get().DA.WindowMedian0, thw);
	return thw.first;
}

uint8_t NodeProcessor::ValidateTxContextEx(const Transaction& tx, const HeightRange& hr, bool bShieldedTested)
{
	Height h = m_Cursor.m_ID.m_Height + 1;

	if (!hr.IsInRange(h))
		return proto::TxStatus::InvalidContext;

	// Cheap tx verification. No need to update the internal structure, recalculate definition, or etc.

	// Ensure input UTXOs are present
	for (size_t i = 0; i < tx.m_vInputs.size(); i++)
	{
		Input::Count nCount = 1;
		const Input& v = *tx.m_vInputs[i];

		for (; i + 1 < tx.m_vInputs.size(); i++, nCount++)
			if (tx.m_vInputs[i + 1]->m_Commitment != v.m_Commitment)
				break;

		if (!ValidateInputs(v.m_Commitment, nCount))
			return proto::TxStatus::InvalidInput; // some input UTXOs are missing
	}

	// Ensure kernels are ok
	BlockInterpretCtx bic(h, true);
	bic.SetAssetHi(*this);
	bic.m_ValidateOnly = true;
	bic.m_UpdateMmrs = false;
	bic.m_SaveKid = false;

	BlockInterpretCtx::BlobSet setDups;
	bic.m_pDups = &setDups;

	BlockInterpretCtx::BlobPtrSet setKrnIds;
	bic.m_pDupIDs = &setKrnIds;

	size_t n = 0;
	if (!HandleElementVecFwd(tx.m_vKernels, bic, n))
		return bic.m_LimitExceeded ? proto::TxStatus::LimitExceeded : proto::TxStatus::InvalidContext;

	// Ensure output assets are in range
	for (size_t i = 0; i < tx.m_vOutputs.size(); i++)
		if (!bic.ValidateAssetRange(tx.m_vOutputs[i]->m_pAsset))
			return proto::TxStatus::InvalidContext;

	if (!bShieldedTested)
	{
		if (bic.m_ShieldedIns)
		{
			assert(bic.m_ShieldedIns <= Rules::get().Shielded.MaxIns);

			ECC::InnerProduct::BatchContextEx<4> bc;
			MultiShieldedContext msc;

			if (!msc.IsValid(tx, bc, 0, 1))
				return proto::TxStatus::InvalidInput;

			msc.Calculate(bc.m_Sum, *this);

			if (!bc.Flush())
				return proto::TxStatus::InvalidInput;
		}

		assert(bic.m_ShieldedOuts <= Rules::get().Shielded.MaxOuts);
	}

	return proto::TxStatus::Ok;
}

bool NodeProcessor::ValidateInputs(const ECC::Point& comm, Input::Count nCount /* = 1 */)
{
	struct Traveler :public UtxoTree::ITraveler
	{
		uint32_t m_Count;
		virtual bool OnLeaf(const RadixTree::Leaf& x) override
		{
			const UtxoTree::MyLeaf& n = Cast::Up<UtxoTree::MyLeaf>(x);
			Input::Count nCount = n.get_Count();
			assert(m_Count && nCount);
			if (m_Count <= nCount)
				return false; // stop iteration

			m_Count -= nCount;
			return true;
		}
	} t;
	t.m_Count = nCount;


	UtxoTree::Key kMin, kMax;

	UtxoTree::Key::Data d;
	d.m_Commitment = comm;
	d.m_Maturity = 0;
	kMin = d;
	d.m_Maturity = m_Cursor.m_ID.m_Height;
	kMax = d;

	UtxoTree::Cursor cu;
	t.m_pCu = &cu;
	t.m_pBound[0] = kMin.V.m_pData;
	t.m_pBound[1] = kMax.V.m_pData;

	return !m_Utxos.Traverse(t);
}

size_t NodeProcessor::GenerateNewBlockInternal(BlockContext& bc, BlockInterpretCtx& bic)
{
	Height h = m_Cursor.m_Sid.m_Height + 1;

	// Generate the block up to the allowed size.
	// All block elements are serialized independently, their binary size can just be added to the size of the "empty" block.

	SerializerSizeCounter ssc;
	ssc & bc.m_Block;

	Block::Builder bb(bc.m_SubIdx, bc.m_Coin, bc.m_Tag, h);

	Output::Ptr pOutp;
	TxKernel::Ptr pKrn;

	bb.AddCoinbaseAndKrn(pOutp, pKrn);
	if (pOutp)
		ssc & *pOutp;
	yas::detail::SaveKrn(ssc, *pKrn, false); // pessimistic

	ECC::Scalar::Native offset = bc.m_Block.m_Offset;

	if (BlockContext::Mode::Assemble != bc.m_Mode)
	{
		if (pOutp)
		{
			if (!HandleBlockElement(*pOutp, bic))
				return 0;

			bc.m_Block.m_vOutputs.push_back(std::move(pOutp));
		}

		if (!HandleBlockElement(*pKrn, bic))
			return 0;

		bc.m_Block.m_vKernels.push_back(std::move(pKrn));
	}

	// estimate the size of the fees UTXO
	if (!m_nSizeUtxoComission)
	{
		Output outp;
		outp.m_pConfidential.reset(new ECC::RangeProof::Confidential);
		ZeroObject(*outp.m_pConfidential);

		SerializerSizeCounter ssc2;
		ssc2 & outp;
		m_nSizeUtxoComission = ssc2.m_Counter.m_Value;
	}

	if (bc.m_Fees)
		ssc.m_Counter.m_Value += m_nSizeUtxoComission;

	const size_t nSizeMax = Rules::get().MaxBodySize;
	if (ssc.m_Counter.m_Value > nSizeMax)
	{
		// the block may be non-empty (i.e. contain treasury)
		LOG_WARNING() << "Block too large.";
		return 0; //
	}

	size_t nTxNum = 0;

	for (TxPool::Fluff::ProfitSet::iterator it = bc.m_TxPool.m_setProfit.begin(); bc.m_TxPool.m_setProfit.end() != it; )
	{
		TxPool::Fluff::Element& x = (it++)->get_ParentObj();

		if (AmountBig::get_Hi(x.m_Profit.m_Fee))
		{
			// huge fees are unsupported
			bc.m_TxPool.Delete(x);
			continue;
		}

		Amount feesNext = bc.m_Fees + AmountBig::get_Lo(x.m_Profit.m_Fee);
		if (feesNext < bc.m_Fees)
			continue; // huge fees are unsupported

		size_t nSizeNext = ssc.m_Counter.m_Value + x.m_Profit.m_nSize;
		if (!bc.m_Fees && feesNext)
			nSizeNext += m_nSizeUtxoComission;

		if (nSizeNext > nSizeMax)
		{
			if (bc.m_Block.m_vInputs.empty() &&
				(bc.m_Block.m_vOutputs.size() == 1) &&
				(bc.m_Block.m_vKernels.size() == 1))
			{
				// won't fit in empty block
				LOG_INFO() << "Tx is too big.";
				bc.m_TxPool.Delete(x);
			}
			continue;
		}

		Transaction& tx = *x.m_pValue;

		bool bDelete = !x.m_Threshold.m_Height.IsInRange(bic.m_Height);
		if (!bDelete)
		{
			assert(!bic.m_LimitExceeded);
			if (HandleValidatedTx(tx, bic))
			{
				TxVectors::Writer(bc.m_Block, bc.m_Block).Dump(tx.get_Reader());

				bc.m_Fees = feesNext;
				ssc.m_Counter.m_Value = nSizeNext;
				offset += ECC::Scalar::Native(tx.m_Offset);
				++nTxNum;
			}
			else
			{
				if (bic.m_LimitExceeded)
					bic.m_LimitExceeded = false; // don't delete it, leave it for the next block
				else
					bDelete = true;
			}
		}

		if (bDelete)
			bc.m_TxPool.Delete(x); // isn't available in this context
	}

	LOG_INFO() << "GenerateNewBlock: size of block = " << ssc.m_Counter.m_Value << "; amount of tx = " << nTxNum;

	if (BlockContext::Mode::Assemble != bc.m_Mode)
	{
		if (bc.m_Fees)
		{
			bb.AddFees(bc.m_Fees, pOutp);
			if (!HandleBlockElement(*pOutp, bic))
				return 0;

			bc.m_Block.m_vOutputs.push_back(std::move(pOutp));
		}

		bb.m_Offset = -bb.m_Offset;
		offset += bb.m_Offset;
	}

	bc.m_Block.m_Offset = offset;

	return ssc.m_Counter.m_Value;
}

void NodeProcessor::GenerateNewHdr(BlockContext& bc)
{
	bc.m_Hdr.m_Prev = m_Cursor.m_ID.m_Hash;
	bc.m_Hdr.m_Height = m_Cursor.m_ID.m_Height + 1;

	Evaluator ev(*this);
	ev.m_Height++;
	ev.get_Definition(bc.m_Hdr.m_Definition);

#ifndef NDEBUG
	// kernels must be sorted already
	for (size_t i = 1; i < bc.m_Block.m_vKernels.size(); i++)
	{
		const TxKernel& krn0 = *bc.m_Block.m_vKernels[i - 1];
		const TxKernel& krn1 = *bc.m_Block.m_vKernels[i];
		assert(krn0 <= krn1);
	}
#endif // NDEBUG

	KrnFlyMmr fmmr(bc.m_Block);
	fmmr.get_Hash(bc.m_Hdr.m_Kernels);

	bc.m_Hdr.m_PoW.m_Difficulty = m_Cursor.m_DifficultyNext;
	bc.m_Hdr.m_TimeStamp = getTimestamp();

	bc.m_Hdr.m_ChainWork = m_Cursor.m_Full.m_ChainWork + bc.m_Hdr.m_PoW.m_Difficulty;

	// Adjust the timestamp to be no less than the moving median (otherwise the block'll be invalid)
	Timestamp tm = get_MovingMedian() + 1;
	std::setmax(bc.m_Hdr.m_TimeStamp, tm);
}

NodeProcessor::BlockContext::BlockContext(TxPool::Fluff& txp, Key::Index nSubKey, Key::IKdf& coin, Key::IPKdf& tag)
	:m_TxPool(txp)
	,m_SubIdx(nSubKey)
	,m_Coin(coin)
	,m_Tag(tag)
{
	m_Fees = 0;
	m_Block.ZeroInit();
}

bool NodeProcessor::GenerateNewBlock(BlockContext& bc)
{
	BlockInterpretCtx bic(m_Cursor.m_Sid.m_Height + 1, true);
	bic.m_UpdateMmrs = false;
	bic.SetAssetHi(*this);

	ByteBuffer bbR;
	bic.m_pRollback = &bbR;

	size_t nSizeEstimated = 1;

	if (BlockContext::Mode::Finalize == bc.m_Mode)
	{
		if (!HandleValidatedTx(bc.m_Block, bic))
			return false;
	}
	else
		nSizeEstimated = GenerateNewBlockInternal(bc, bic);

	bic.m_Fwd = false;
    BEAM_VERIFY(HandleValidatedTx(bc.m_Block, bic)); // undo changes
	assert(bbR.empty());

	// reset input maturities
	for (size_t i = 0; i < bc.m_Block.m_vInputs.size(); i++)
		bc.m_Block.m_vInputs[i]->m_Internal.m_Maturity = 0;

	if (!nSizeEstimated)
		return false;

	if (BlockContext::Mode::Assemble == bc.m_Mode)
	{
		bc.m_Hdr.m_Height = bic.m_Height;
		return true;
	}

	size_t nCutThrough = bc.m_Block.Normalize(); // right before serialization
	nCutThrough; // remove "unused var" warning

	// The effect of the cut-through block may be different than it was during block construction, because the consumed and created UTXOs (removed by cut-through) could have different maturities.
	// Hence - we need to re-apply the block after the cut-throught, evaluate the definition, and undo the changes (once again).
	//
	// In addition to this, kernels reorder may also have effect: shielded outputs may get different IDs
	bic.m_Fwd = true;
	bic.m_AlreadyValidated = true;
	bic.m_SaveKid = false;
	bic.m_UpdateMmrs = true;

	bool bOk = HandleValidatedTx(bc.m_Block, bic);
	if (!bOk)
	{
		LOG_WARNING() << "couldn't apply block after cut-through!";
		return false; // ?!
	}
	GenerateNewHdr(bc);
	bic.m_Fwd = false;
    BEAM_VERIFY(HandleValidatedTx(bc.m_Block, bic)); // undo changes
	assert(bbR.empty());

	Serializer ser;

	ser.reset();
	ser & Cast::Down<Block::BodyBase>(bc.m_Block);
	ser & Cast::Down<TxVectors::Perishable>(bc.m_Block);
	ser.swap_buf(bc.m_BodyP);

	ser.reset();
	ser & Cast::Down<TxVectors::Eternal>(bc.m_Block);
	ser.swap_buf(bc.m_BodyE);

	size_t nSize = bc.m_BodyP.size() + bc.m_BodyE.size();

	if (BlockContext::Mode::SinglePass == bc.m_Mode)
	{
		// the actual block size may be less because of:
		// 1. Cut-through removed some data
		// 2. our size estimation is a little pessimistic because of extension of kernels. If all kernels are standard, then 1 bytes per kernel is saved
		assert(nCutThrough ?
			(nSize < nSizeEstimated) :
			(
				(nSize == nSizeEstimated) ||
				(nSize == nSizeEstimated - bc.m_Block.m_vKernels.size())
			)
		);
	}

	return nSize <= Rules::get().MaxBodySize;
}

Executor& NodeProcessor::get_Executor()
{
	if (!m_pExecSync)
	{
		m_pExecSync = std::make_unique<MyExecutor>();
		m_pExecSync->m_Ctx.m_pThis = m_pExecSync.get();
		m_pExecSync->m_Ctx.m_iThread = 0;
	}

	return *m_pExecSync;
}

uint32_t NodeProcessor::MyExecutor::get_Threads()
{
	return 1;
}

void NodeProcessor::MyExecutor::Push(TaskAsync::Ptr&& pTask)
{
	ExecAll(*pTask);
}

uint32_t NodeProcessor::MyExecutor::Flush(uint32_t)
{
	return 0;
}

void NodeProcessor::MyExecutor::ExecAll(TaskSync& t)
{
	ECC::InnerProduct::BatchContext::Scope scope(m_Ctx.m_BatchCtx);
	t.Exec(m_Ctx);
}

bool NodeProcessor::ValidateAndSummarize(TxBase::Context& ctx, const TxBase& txb, TxBase::IReader&& r)
{
	struct MyShared
		:public MultiblockContext::MyTask::Shared
	{
		TxBase::Context::Params m_Pars;
		TxBase::Context* m_pCtx;
		const TxBase* m_pTx;
		TxBase::IReader* m_pR;

		MyShared(MultiblockContext& mbc)
			:MultiblockContext::MyTask::Shared(mbc)
		{
		}

		virtual ~MyShared() {} // auto

		virtual void Exec(uint32_t iThread) override
		{
			TxBase::Context ctx(m_Pars);
			ctx.m_Height = m_pCtx->m_Height;
			ctx.m_iVerifier = iThread;

			TxBase::IReader::Ptr pR;
			m_pR->Clone(pR);

			bool bValid = ctx.ValidateAndSummarize(*m_pTx, std::move(*pR));

			std::unique_lock<std::mutex> scope(m_Mbc.m_Mutex);

			if (bValid && !m_Mbc.m_bFail)
				bValid = m_pCtx->Merge(ctx);

			if (!bValid)
				m_Mbc.m_bFail = true;
		}
	};

	MultiblockContext mbc(*this);

	std::shared_ptr<MyShared> pShared = std::make_shared<MyShared>(mbc);

	pShared->m_Pars = ctx.m_Params;
	pShared->m_pCtx = &ctx;
	pShared->m_pTx = &txb;
	pShared->m_pR = &r;

	mbc.m_InProgress.m_Max++; // dummy, just to emulate ongoing progress
	mbc.PushTasks(pShared, pShared->m_Pars);

	return mbc.Flush();
}

bool NodeProcessor::ExtractBlockWithExtra(Block::Body& block, const NodeDB::StateID& sid)
{
	ByteBuffer bbE;
	if (!GetBlockInternal(sid, &bbE, nullptr, 0, 0, 0, false, &block))
		return false;

	Deserializer der;
	der.reset(bbE);
	der & Cast::Down<TxVectors::Eternal>(block);

	// Set maturity to inputs
	for (size_t i = 0; i < block.m_vInputs.size(); i++)
	{
		Input& inp = *block.m_vInputs[i];
		ToInputWithMaturity(inp, inp.m_Internal.m_ID);
	}

	return true;
}

TxoID NodeProcessor::get_TxosBefore(Height h)
{
	if (h < Rules::HeightGenesis)
		return 0;

	if (Rules::HeightGenesis == h)
		return m_Extra.m_TxosTreasury;

	TxoID id = m_DB.get_StateTxos(FindActiveAtStrict(h - 1));
	if (MaxHeight == id)
		OnCorrupted();

	return id;
}

TxoID NodeProcessor::FindHeightByTxoID(Height& h, TxoID id0)
{
	if (id0 < m_Extra.m_TxosTreasury)
	{
		h = 0;
		return m_Extra.m_TxosTreasury;
	}

	NodeDB::StateID sid;
	TxoID ret = m_DB.FindStateByTxoID(sid, id0);

	h = sid.m_Height;
	return ret;
}

bool NodeProcessor::EnumTxos(ITxoWalker& wlk)
{
	return EnumTxos(wlk, HeightRange(Rules::HeightGenesis - 1, m_Cursor.m_ID.m_Height));
}

bool NodeProcessor::EnumTxos(ITxoWalker& wlkTxo, const HeightRange& hr)
{
	if (hr.IsEmpty())
		return true;
	assert(hr.m_Max <= m_Cursor.m_ID.m_Height);

	TxoID id1 = get_TxosBefore(hr.m_Min);
	Height h = hr.m_Min - 1; // don't care about overflow

	NodeDB::WalkerTxo wlk;
	for (m_DB.EnumTxos(wlk, id1);  wlk.MoveNext(); )
	{
		if (wlk.m_ID >= id1)
		{
			if (++h > hr.m_Max)
				break;

			if (h < Rules::HeightGenesis)
				id1 = m_Extra.m_TxosTreasury;

			if (wlk.m_ID >= id1)
			{
				id1 = FindHeightByTxoID(h, wlk.m_ID);
				assert(wlk.m_ID < id1);
			}
		}

		if (!wlkTxo.OnTxo(wlk, h))
			return false;
	}

	return true;
}

bool NodeProcessor::EnumKernels(IKrnWalker& wlkKrn, const HeightRange& hr)
{
	if (hr.IsEmpty())
		return true;
	assert(hr.m_Max <= m_Cursor.m_ID.m_Height);

	ByteBuffer bbE;
	TxVectors::Eternal txve;

	m_Extra.m_ShieldedOutputs = 0;

	for (wlkKrn.m_Height = hr.m_Min; wlkKrn.m_Height <= hr.m_Max; wlkKrn.m_Height++)
	{
		uint64_t row = FindActiveAtStrict(wlkKrn.m_Height);
		m_DB.GetStateBlock(row, nullptr, &bbE, nullptr);

		Deserializer der;
		der.reset(bbE);
		der & txve;

		if (!wlkKrn.Process(txve.m_vKernels))
			return false;
	}

	return true;
}

bool NodeProcessor::ITxoWalker::OnTxo(const NodeDB::WalkerTxo& wlk , Height hCreate)
{
	Deserializer der;
	der.reset(wlk.m_Value.p, wlk.m_Value.n);

	Output outp;
	der & outp;

	return OnTxo(wlk, hCreate, outp);
}

bool NodeProcessor::ITxoWalker::OnTxo(const NodeDB::WalkerTxo&, Height hCreate, Output&)
{
	assert(false);
	return false;
}

bool NodeProcessor::ITxoRecover::OnTxo(const NodeDB::WalkerTxo& wlk, Height hCreate)
{
	if (TxoIsNaked(wlk.m_Value))
		return true;

	return ITxoWalker::OnTxo(wlk, hCreate);
}

bool NodeProcessor::ITxoRecover::OnTxo(const NodeDB::WalkerTxo& wlk, Height hCreate, Output& outp)
{
	CoinID cid;
	if (!outp.Recover(hCreate, m_Key, cid))
		return true;

	return OnTxo(wlk, hCreate, outp, cid);
}

bool NodeProcessor::ITxoWalker_UnspentNaked::OnTxo(const NodeDB::WalkerTxo& wlk, Height hCreate)
{
	if (wlk.m_SpendHeight != MaxHeight)
		return true;

	uint8_t pNaked[s_TxoNakedMax];
	TxoToNaked(pNaked, Cast::NotConst(wlk).m_Value); // save allocation and deserialization of sig

	return ITxoWalker::OnTxo(wlk, hCreate);
}

bool NodeProcessor::ITxoWalker_Unspent::OnTxo(const NodeDB::WalkerTxo& wlk, Height hCreate)
{
	if (wlk.m_SpendHeight != MaxHeight)
		return true;

	return ITxoWalker::OnTxo(wlk, hCreate);
}

void NodeProcessor::InitializeUtxos()
{
	assert(!m_Extra.m_Txos);

	struct Walker
		:public ITxoWalker_UnspentNaked
	{
		TxoID m_TxosTotal;
		NodeProcessor& m_This;
		Walker(NodeProcessor& x) :m_This(x) {}

		virtual bool OnTxo(const NodeDB::WalkerTxo& wlk, Height hCreate) override
		{
			m_This.InitializeUtxosProgress(wlk.m_ID, m_TxosTotal);
			return ITxoWalker_UnspentNaked::OnTxo(wlk, hCreate);
		}

		virtual bool OnTxo(const NodeDB::WalkerTxo& wlk, Height hCreate, Output& outp) override
		{
			m_This.m_Extra.m_Txos = wlk.m_ID;
			BlockInterpretCtx bic(hCreate, true);
			if (!m_This.HandleBlockElement(outp, bic))
				OnCorrupted();

			return true;
		}
	};

	Walker wlk(*this);
	wlk.m_TxosTotal = get_TxosBefore(m_Cursor.m_ID.m_Height + 1);
	EnumTxos(wlk);
}

bool NodeProcessor::GetBlock(const NodeDB::StateID& sid, ByteBuffer* pEthernal, ByteBuffer* pPerishable, Height h0, Height hLo1, Height hHi1, bool bActive)
{
	return GetBlockInternal(sid, pEthernal, pPerishable, h0, hLo1, hHi1, bActive, nullptr);
}

bool NodeProcessor::GetBlockInternal(const NodeDB::StateID& sid, ByteBuffer* pEthernal, ByteBuffer* pPerishable, Height h0, Height hLo1, Height hHi1, bool bActive, Block::Body* pBody)
{
	// h0 - current peer Height
	// hLo1 - HorizonLo that peer needs after the sync
	// hHi1 - HorizonL1 that peer needs after the sync
	if ((hLo1 > hHi1) || (h0 >= sid.m_Height))
		return false;

	// For every output:
	//	if SpendHeight > hHi1 (or null) then fully transfer
	//	if SpendHeight > hLo1 then transfer naked (remove Confidential, Public, Asset::ID)
	//	Otherwise - don't transfer

	// For every input (commitment only):
	//	if SpendHeight > hLo1 then transfer
	//	if CreateHeight <= h0 then transfer
	//	Otherwise - don't transfer

	std::setmax(hHi1, sid.m_Height); // valid block can't spend its own output. Hence this means full block should be transferred

	if (m_Extra.m_TxoHi > hHi1)
		return false;

	std::setmax(hLo1, sid.m_Height - 1);
	if (m_Extra.m_TxoLo > hLo1)
		return false;

	if ((h0 >= Rules::HeightGenesis) && (m_Extra.m_TxoLo > sid.m_Height))
		return false; // we don't have any info for the range [Rules::HeightGenesis, h0].

	// in case we're during sync - make sure we don't return non-full blocks as-is
	if (IsFastSync() && (sid.m_Height > m_Cursor.m_ID.m_Height))
		return false;

	bool bFullBlock = (sid.m_Height >= hHi1) && (sid.m_Height > hLo1) && !pBody;
	m_DB.GetStateBlock(sid.m_Row, bFullBlock ? pPerishable : nullptr, pEthernal, nullptr);

	if (!pBody && !(pPerishable && pPerishable->empty()))
		return true;

	// re-create it from Txos
	if (!bActive && !(m_DB.GetStateFlags(sid.m_Row) & NodeDB::StateFlags::Active))
		return false; // only active states are supported

	TxoID idInpCut = get_TxosBefore(h0 + 1);
	TxoID id0;

	TxoID id1 = m_DB.get_StateTxos(sid.m_Row);

	ByteBuffer bbBlob;
	TxBase txb;
	if (!m_DB.get_StateExtra(sid.m_Row, txb.m_Offset))
		OnCorrupted();

	uint64_t rowid = sid.m_Row;
	if (m_DB.get_Prev(rowid))
	{
		AdjustOffset(txb.m_Offset, rowid, false);
		id0 = m_DB.get_StateTxos(rowid);
	}
	else
		id0 = m_Extra.m_TxosTreasury;

	Serializer ser;
	if (pBody)
		Cast::Down<TxBase>(*pBody) = std::move(txb);
	else
		ser & txb;

	uint32_t nCount = 0;

	// inputs
	std::vector<NodeDB::StateInput> v;
	m_DB.get_StateInputs(sid.m_Row, v);

	for (uint32_t iCycle = 0; ; iCycle++)
	{
		for (size_t i = 0; i < v.size(); i++)
		{
			TxoID id = v[i].get_ID();

			//	if SpendHeight > hLo1 then transfer
			//	if CreateHeight <= h0 then transfer
			//	Otherwise - don't transfer
			if ((sid.m_Height > hLo1) || (id < idInpCut))
			{
				if (iCycle)
				{
					const NodeDB::StateInput& si = v[i];

					if (pBody)
					{
						Input::Ptr& pInp = pBody->m_vInputs.emplace_back();
						pInp.reset(new Input);
						si.Get(pInp->m_Commitment);
						pInp->m_Internal.m_ID = si.get_ID();
					}
					else
					{
						// write
						Input inp;
						si.Get(inp.m_Commitment);
						ser & inp;
					}
				}
				else
					nCount++;
			}
		}

		if (iCycle)
			break;

		if (pBody)
			pBody->m_vInputs.reserve(nCount);
		else
			ser & uintBigFrom(nCount);
	}

	nCount = 0;

	// outputs
	if (pBody)
		pBody->m_vOutputs.reserve(static_cast<size_t>(id1 - id0 - 1)); // num of original outputs

	NodeDB::WalkerTxo wlk;
	for (m_DB.EnumTxos(wlk, id0); wlk.MoveNext(); )
	{
		if (wlk.m_ID >= id1)
			break;

		//	if SpendHeight > hHi1 (or null) then fully transfer
		//	if SpendHeight > hLo1 then transfer naked (remove Confidential, Public, Asset::ID)
		//	Otherwise - don't transfer

		if (wlk.m_SpendHeight <= hLo1)
			continue;

		uint8_t pNaked[s_TxoNakedMax];

		if (wlk.m_SpendHeight <= hHi1)
			TxoToNaked(pNaked, wlk.m_Value);

		if (pBody)
		{
			Deserializer der;
			der.reset(wlk.m_Value.p, wlk.m_Value.n);

			Output::Ptr& pOutp = pBody->m_vOutputs.emplace_back();
			pOutp.reset(new Output);
			der & *pOutp;
		}
		else
		{
			nCount++;

			const uint8_t* p = reinterpret_cast<const uint8_t*>(wlk.m_Value.p);
			bbBlob.insert(bbBlob.end(), p, p + wlk.m_Value.n);
		}
	}

	if (!pBody)
	{
		ser & uintBigFrom(nCount);
		ser.swap_buf(*pPerishable);
		pPerishable->insert(pPerishable->end(), bbBlob.begin(), bbBlob.end());
		
		ser.swap_buf(*pPerishable);

		ser.swap_buf(*pPerishable);
	}

	return true;
}

NodeProcessor::RecentStates::Entry& NodeProcessor::RecentStates::get_FromTail(size_t x) const
{
	assert((x < m_Count) && (m_Count <= m_vec.size()));
	return Cast::NotConst(m_vec[(m_i0 + m_Count - x - 1) % m_vec.size()]);
}

const NodeProcessor::RecentStates::Entry* NodeProcessor::RecentStates::Get(Height h) const
{
	if (!m_Count)
		return nullptr;

	const Entry& e = get_FromTail(0);
	if (h > e.m_State.m_Height)
		return nullptr;

	Height dh = e.m_State.m_Height - h;
	if (dh >= m_Count)
		return nullptr;

	const Entry& e2 = get_FromTail(static_cast<size_t>(dh));
	assert(e2.m_State.m_Height == h);
	return &e2;
}

void NodeProcessor::RecentStates::RollbackTo(Height h)
{
	for (; m_Count; m_Count--)
	{
		const Entry& e = get_FromTail(0);
		if (e.m_State.m_Height == h)
			break;
	}
}

void NodeProcessor::RecentStates::Push(uint64_t rowID, const Block::SystemState::Full& s)
{
	if (m_vec.empty())
	{
		// we use this cache mainly to improve difficulty calculation. Hence the cache size is appropriate
		const Rules& r = Rules::get();
	
		const size_t n = std::max(r.DA.WindowWork + r.DA.WindowMedian1, r.DA.WindowMedian0) + 5;
		m_vec.resize(n);
	}
	else
	{
		// ensure we don't have out-of-order entries
		RollbackTo(s.m_Height - 1);
	}

	if (m_Count < m_vec.size())
		m_Count++;
	else
		m_i0++;

	Entry& e = get_FromTail(0);
	e.m_RowID = rowID;
	e.m_State = s;
}

} // namespace beam
