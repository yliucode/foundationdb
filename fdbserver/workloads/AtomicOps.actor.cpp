/*
 * AtomicOps.actor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "fdbrpc/ContinuousSample.h"
#include "fdbclient/NativeAPI.actor.h"
#include "fdbserver/TesterInterface.actor.h"
#include "fdbserver/workloads/BulkSetup.actor.h"
#include "fdbclient/ReadYourWrites.h"
#include "fdbserver/workloads/workloads.actor.h"
#include "flow/actorcompiler.h"  // This must be the last #include.

//#define SevAtomicOpDebug SevInfo
#define SevAtomicOpDebug SevVerbose

struct AtomicOpsWorkload : TestWorkload {
	int opNum, actorCount, nodeCount;
	uint32_t opType;
	bool apiVersion500 = false;
	Key keyPrefix;
	bool fetchKeyPrefixFromDB;

	double testDuration, transactionsPerSecond;
	vector<Future<Void>> clients;
	uint64_t lbsum, ubsum; // The lower bound and upper bound sum of operations when opType = AddValue

	AtomicOpsWorkload(WorkloadContext const& wcx)
		: TestWorkload(wcx), opNum(0)
	{
		testDuration = getOption( options, LiteralStringRef("testDuration"), 600.0 );
		transactionsPerSecond = getOption( options, LiteralStringRef("transactionsPerSecond"), 5000.0 ) / clientCount;
		actorCount = getOption( options, LiteralStringRef("actorsPerClient"), transactionsPerSecond / 5 );
		opType = getOption( options, LiteralStringRef("opType"), -1 );
		nodeCount = getOption( options, LiteralStringRef("nodeCount"), 1000 );
		keyPrefix = unprintable(getOption(options, "keyPrefix"_sr, LiteralStringRef("")).toString());
		fetchKeyPrefixFromDB = getOption(options, "fetchKeyPrefixFromDB"_sr, false);
		// Atomic OPs Min and And have modified behavior from api version 510. Hence allowing testing for older version (500) with a 10% probability
		// Actual change of api Version happens in setup
		apiVersion500 = ((sharedRandomNumber % 10) == 0);
		TraceEvent("AtomicOpsApiVersion500").detail("ApiVersion500", apiVersion500);

		lbsum = 0;
		ubsum = 0;

		int64_t randNum = sharedRandomNumber / 10;
		if (opType == -1) opType = randNum % 10;

		switch(opType) {
		case 0:
			TEST(true); //Testing atomic AddValue
			opType = MutationRef::AddValue;
			break;
		case 1:
			TEST(true); //Testing atomic And
			opType = MutationRef::And;
			break;
		case 2:
			TEST(true); //Testing atomic Or
			opType = MutationRef::Or;
			break;
		case 3:
			TEST(true); //Testing atomic Xor
			opType = MutationRef::Xor;
			break;
		case 4:
			TEST(true); //Testing atomic Max
			opType = MutationRef::Max;
			break;
		case 5:
			TEST(true); //Testing atomic Min
			opType = MutationRef::Min;
			break;
		case 6:
			TEST(true); //Testing atomic ByteMin
			opType = MutationRef::ByteMin;
			break;
		case 7:
			TEST(true); //Testing atomic ByteMax
			opType = MutationRef::ByteMax;
			break;
		case 8:
			TEST(true); // Testing atomic MinV2
			opType = MutationRef::MinV2;
			break;
		case 9:
			TEST(true); // Testing atomic AndV2
			opType = MutationRef::AndV2;
			break;
		// case 10:
		// 	TEST(true); // Testing atomic CompareAndClear Not supported yet
		// 	opType = MutationRef::CompareAndClear
		//  break;
		default:
			ASSERT(false);
		}
		TraceEvent("AtomicWorkload").detail("OpType", opType);
	}

	virtual std::string description() { return "AtomicOps"; }

	virtual Future<Void> setup( Database const& cx ) {
		if (apiVersion500)
			cx->apiVersion = 500;

		if(clientId != 0)
			return Void();
		return _setup( cx, this );
	}

	virtual Future<Void> start( Database const& cx ) {
		for (int c = 0; c < actorCount; c++) {
			clients.push_back(
			    timeout(atomicOpWorker(cx->clone(), this, actorCount / transactionsPerSecond), testDuration, Void()));
		}

		return delay(testDuration);
	}

	virtual Future<bool> check( Database const& cx ) {
		if(clientId != 0)
			return true;
		return _check( cx, this );
	}

	virtual void getMetrics( vector<PerfMetric>& m ) {
	}

	std::pair<Key, Key> logDebugKey(int group) {
		Key logKey(format("log%08x%08x%08x", group, clientId, opNum));
		Key debugKey(format("debug%08x%08x%08x", group, clientId, opNum));
		opNum++;
		return std::make_pair(logKey.withPrefix(keyPrefix), debugKey.withPrefix(keyPrefix));
	}

	ACTOR Future<Void> _setup( Database cx, AtomicOpsWorkload* self ) {
		if (self->fetchKeyPrefixFromDB) {
			Key _keyPrefix = wait(self->waitForWorkloadKeyPrefix(cx));
			self->keyPrefix = _keyPrefix;
		}

		// Sanity check if log keyspace has elements
		state ReadYourWritesTransaction tr1(cx);
		loop {
			try {
				Key begin = Key(std::string("log")).withPrefix(self->keyPrefix);
				Standalone<RangeResultRef> log =
				    wait(tr1.getRange(KeyRangeRef(begin, strinc(begin)), CLIENT_KNOBS->TOO_MANY));
				if (!log.empty()) {
					TraceEvent(SevError, "AtomicOpSetup")
					    .detail("LogKeySpace", "Not empty")
					    .detail("Result", log.toString());
					for (auto& kv : log) {
						TraceEvent(SevWarn, "AtomicOpSetup")
						    .detail("K", kv.key.toString())
						    .detail("V", kv.value.toString());
					}
				}
				break;
			} catch (Error& e) {
				wait(tr1.onError(e));
			}
		}

		state int g = 0;
		for(; g < 100; g++) {
			state ReadYourWritesTransaction tr(cx);
			loop {
				try {
					for(int i = 0; i < self->nodeCount/100; i++) {
						uint64_t intValue = 0;
						tr.set(Key(format("ops%08x%08x", g, i)).withPrefix(self->keyPrefix),
						       StringRef((const uint8_t*)&intValue, sizeof(intValue)));
					}
					wait( tr.commit() );
					break;
				} catch( Error &e ) {
					wait( tr.onError(e) );
				}
			}
		}
		return Void();
	}

	ACTOR Future<Void> atomicOpWorker( Database cx, AtomicOpsWorkload* self, double delay ) {
		state double lastTime = now();
		loop {
			wait( poisson( &lastTime, delay ) );
			state ReadYourWritesTransaction tr(cx);
			loop {
				int group = deterministicRandom()->randomInt(0, 100);
				state uint64_t intValue = deterministicRandom()->randomInt(0, 10000000);
				state Key val = StringRef((const uint8_t*)&intValue, sizeof(intValue));
				state std::pair<Key, Key> logDebugKey = self->logDebugKey(group);
				int nodeIndex = deterministicRandom()->randomInt(0, self->nodeCount / 100);
				state Key opsKey = Key(format("ops%08x%08x", group, nodeIndex)).withPrefix(self->keyPrefix);
				try {
					tr.set(logDebugKey.first, val); // set log key
					tr.set(logDebugKey.second, opsKey); // set debug key; one opsKey can have multiple logs key
					tr.atomicOp(opsKey, val, self->opType);
					wait( tr.commit() );
					TraceEvent(SevAtomicOpDebug, "AtomicOpWorker")
					    .detail("OpsKey", opsKey)
					    .detail("LogKey", logDebugKey.first)
					    .detail("Value", val.toString());
					if (self->opType == MutationRef::AddValue) {
						self->lbsum += intValue;
						self->ubsum += intValue;
					}
					break;
				} catch( Error &e ) {
					if (e.code() == 1021) {
						self->ubsum += intValue;
						TraceEvent(SevInfo, "TxnCommitUnknownResult")
						    .detail("Value", intValue)
						    .detail("LogKey", logDebugKey.first)
						    .detail("OpsKey", opsKey);
					}
					wait(tr.onError(e));
				}
			}
		}
	}

	ACTOR Future<Void> dumpLogKV(Database cx, Key keyPrefix, int g) {
		state ReadYourWritesTransaction tr(cx);
		try {
			Key begin = Key(format("log%08x", g)).withPrefix(keyPrefix);
			Standalone<RangeResultRef> log = wait(tr.getRange(KeyRangeRef(begin, strinc(begin)), CLIENT_KNOBS->TOO_MANY));
			if (log.more) {
				TraceEvent(SevError, "LogHitTxnLimits").detail("Result", log.toString());
			}
			uint64_t sum = 0;
			for (auto& kv : log) {
				uint64_t intValue = 0;
				memcpy(&intValue, kv.value.begin(), kv.value.size());
				sum += intValue;
				TraceEvent("AtomicOpLog")
					.detail("Key", kv.key)
					.detail("Val", kv.value)
					.detail("IntValue", intValue)
					.detail("CurSum", sum);
			}
		} catch( Error &e ) {
			TraceEvent("DumpLogKVError").detail("Error", e.what());
			wait( tr.onError(e) );
		}
		return Void();
	}

	ACTOR Future<Void> dumpDebugKV(Database cx, Key keyPrefix, int g) {
		state ReadYourWritesTransaction tr(cx);
		try {
			Key begin = Key(format("debug%08x", g)).withPrefix(keyPrefix);
			Standalone<RangeResultRef> debuglog =
			    wait(tr.getRange(KeyRangeRef(begin, strinc(begin)), CLIENT_KNOBS->TOO_MANY));
			if (debuglog.more) {
				TraceEvent(SevError, "DebugLogHitTxnLimits").detail("Result", debuglog.toString());
			}
			for (auto& kv : debuglog) {
				TraceEvent("AtomicOpDebug").detail("Key", kv.key).detail("Val", kv.value);
			}
		} catch( Error &e ) {
			TraceEvent("DumpDebugKVError").detail("Error", e.what());
			wait( tr.onError(e) );
		}
		return Void();
	}

	ACTOR Future<Void> dumpOpsKV(Database cx, Key keyPrefix, int g) {
		state ReadYourWritesTransaction tr(cx);
		try {
			Key begin = Key(format("ops%08x", g)).withPrefix(keyPrefix);
			Standalone<RangeResultRef> ops =
			    wait(tr.getRange(KeyRangeRef(begin, strinc(begin)), CLIENT_KNOBS->TOO_MANY));
			if (ops.more) {
				TraceEvent(SevError, "OpsHitTxnLimits").detail("Result", ops.toString());
			}
			uint64_t sum = 0;
			for (auto& kv : ops) {
				uint64_t intValue = 0;
				memcpy(&intValue, kv.value.begin(), kv.value.size());
				sum += intValue;
				TraceEvent("AtomicOpOps")
				    .detail("Key", kv.key)
				    .detail("Val", kv.value)
				    .detail("IntVal", intValue)
				    .detail("CurSum", sum);
			}
		} catch (Error& e) {
			TraceEvent("DumpOpsKVError").detail("Error", e.what());
			wait(tr.onError(e));
		}
		return Void();
	}

	ACTOR Future<Void> validateOpsKey(Database cx, AtomicOpsWorkload* self, int g) {
		// Get mapping between opsKeys and debugKeys
		state ReadYourWritesTransaction tr1(cx);
		state std::map<Key, Key> records; // <ops, debugKey>
		Standalone<RangeResultRef> debuglog = wait(
		    tr1.getRange(prefixRange(Key(format("debug%08x", g)).withPrefix(self->keyPrefix)), CLIENT_KNOBS->TOO_MANY));
		if (debuglog.more) {
			TraceEvent(SevError, "DebugLogHitTxnLimits").detail("Result", debuglog.toString());
			return Void();
		}
		for (auto& kv : debuglog) {
			records[kv.value] = kv.key;
		}

		// Get log key's value and assign it to the associated debugKey
		state ReadYourWritesTransaction tr2(cx);
		state std::map<Key, int64_t> logVal; // debugKey, log's value
		Standalone<RangeResultRef> log = wait(
		    tr2.getRange(prefixRange(Key(format("log%08x", g)).withPrefix(self->keyPrefix)), CLIENT_KNOBS->TOO_MANY));
		if (log.more) {
			TraceEvent(SevError, "LogHitTxnLimits").detail("Result", log.toString());
			return Void();
		}
		for (auto& kv : log) {
			uint64_t intValue = 0;
			memcpy(&intValue, kv.value.begin(), kv.value.size());
			logVal[kv.key.removePrefix(self->keyPrefix)
			           .removePrefix(LiteralStringRef("log"))
			           .withPrefix(LiteralStringRef("debug"))
			           .withPrefix(self->keyPrefix)] = intValue;
		}

		// Get opsKeys and validate if it has correct value
		state ReadYourWritesTransaction tr3(cx);
		state std::map<Key, int64_t> opsVal; // ops key, ops value
		Standalone<RangeResultRef> ops = wait(
		    tr3.getRange(prefixRange(Key(format("ops%08x", g)).withPrefix(self->keyPrefix)), CLIENT_KNOBS->TOO_MANY));
		if (ops.more) {
			TraceEvent(SevError, "OpsHitTxnLimits").detail("Result", ops.toString());
			return Void();
		}
		// Validate if ops' key value is consistent with logs' key value
		for (auto& kv : ops) {
			bool inRecord = records.find(kv.key) != records.end();
			uint64_t intValue = 0;
			memcpy(&intValue, kv.value.begin(), kv.value.size());
			opsVal[kv.key] = intValue;
			if (!inRecord) {
				TraceEvent(SevWarnAlways, "MissingLogKey").detail("OpsKey", kv.key);
			}
			if (inRecord && (self->actorCount == 1 && intValue != logVal[records[kv.key]])) {
				// When multiple actors exist, 1 opsKey can have multiple log keys
				TraceEvent(SevError, "InconsistentOpsKeyValue")
				    .detail("OpsKey", kv.key)
				    .detail("DebugKey", records[kv.key])
				    .detail("LogValue", logVal[records[kv.key]])
				    .detail("OpValue", intValue);
			}
		}

		// Validate if there is any ops key missing
		for (auto& kv : records) {
			if (opsVal.find(kv.first) == opsVal.end()) {
				TraceEvent(SevError, "MissingOpsKey2")
				    .detail("OpsKey", kv.first)
				    .detail("DebugKey", kv.second);
			}
		}
		return Void();
	}

	ACTOR Future<bool> _check( Database cx, AtomicOpsWorkload* self ) {
		state int g = 0;
		state bool ret = true;
		for(; g < 100; g++) {
			state ReadYourWritesTransaction tr(cx);
			state Standalone<RangeResultRef> log;
			state Key logResultKey = Key(LiteralStringRef("xlogResult")).withPrefix(self->keyPrefix);
			state Key opsResultKey = Key(LiteralStringRef("xopsResult")).withPrefix(self->keyPrefix);
			loop {
				try {
					{
						// Calculate the accumulated value in the log keyspace for the group g
						Key begin = Key(format("log%08x", g)).withPrefix(self->keyPrefix);
						Standalone<RangeResultRef> log_ = wait( tr.getRange(KeyRangeRef(begin, strinc(begin)), CLIENT_KNOBS->TOO_MANY) );
						log = log_;
						uint64_t zeroValue = 0;
						tr.set(logResultKey, StringRef((const uint8_t*)&zeroValue, sizeof(zeroValue)));
						for(auto& kv : log) {
							uint64_t intValue = 0;
							memcpy(&intValue, kv.value.begin(), kv.value.size());
							tr.atomicOp(logResultKey, kv.value, self->opType);
						}
					}

					{
						// Calculate the accumulated value in the ops keyspace for the group g
						Key begin = Key(format("ops%08x", g)).withPrefix(self->keyPrefix);
						Standalone<RangeResultRef> ops = wait( tr.getRange(KeyRangeRef(begin, strinc(begin)), CLIENT_KNOBS->TOO_MANY) );
						uint64_t zeroValue = 0;
						tr.set(opsResultKey, StringRef((const uint8_t*)&zeroValue, sizeof(zeroValue)));
						for(auto& kv : ops) {
							uint64_t intValue = 0;
							memcpy(&intValue, kv.value.begin(), kv.value.size());
							tr.atomicOp(opsResultKey, kv.value, self->opType);
						}

						if (tr.get(logResultKey).get() != tr.get(opsResultKey).get()) {
							Optional<Standalone<StringRef>> logResult = tr.get(logResultKey).get();
							Optional<Standalone<StringRef>> opsResult = tr.get(opsResultKey).get();
							ASSERT(logResult.present());
							ASSERT(opsResult.present());
							// here
							TraceEvent(SevError, "LogMismatch")
							    .detail("Index", format("log%08x", g))
							    .detail("LogResult", printable(logResult))
							    .detail("OpsResult", printable(opsResult));
						}

						if( self->opType == MutationRef::AddValue ) {
							uint64_t opsResult=0;
							Key opsResultStr = tr.get(opsResultKey).get().get();
							memcpy(&opsResult, opsResultStr.begin(), opsResultStr.size());
							uint64_t logResult=0;
							for(auto& kv : log) {
								uint64_t intValue = 0;
								memcpy(&intValue, kv.value.begin(), kv.value.size());
								logResult += intValue;
							}
							if(logResult != opsResult) {
								TraceEvent(SevError, "LogAddMismatch")
								    .detail("LogResult", logResult)
								    .detail("OpResult", opsResult)
								    .detail("OpsResultStr", printable(opsResultStr))
								    .detail("Size", opsResultStr.size())
								    .detail("LowerBoundSum", self->lbsum)
								    .detail("UpperBoundSum", self->ubsum);
								wait(self->dumpLogKV(cx, self->keyPrefix, g));
								wait(self->dumpDebugKV(cx, self->keyPrefix, g));
								wait(self->dumpOpsKV(cx, self->keyPrefix, g));
								wait(self->validateOpsKey(cx, self, g));
							}
						}
						break;
					}
				} catch( Error &e ) {
					wait( tr.onError(e) );
				}
			}
		}
		return ret;
	}
};

WorkloadFactory<AtomicOpsWorkload> AtomicOpsWorkloadFactory("AtomicOps");
