////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2011-2014, Armory Technologies, Inc.                        //
//  Distributed under the GNU Affero General Public License (AGPL v3)         //
//  See LICENSE or http://www.gnu.org/licenses/agpl.html                      //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////
//
#ifndef _LMDB_WRAPPER_
#define _LMDB_WRAPPER_

#include <list>
#include <vector>
#include "log.h"
#include "BinaryData.h"
#include "BtcUtils.h"
#include "BlockObj.h"
#include "StoredBlockObj.h"

#include "lmdbpp.h"

////////////////////////////////////////////////////////////////////////////////
//
// Create & manage a bunch of different databases
//
////////////////////////////////////////////////////////////////////////////////

#define STD_READ_OPTS       leveldb::ReadOptions()
#define STD_WRITE_OPTS      leveldb::WriteOptions()

#define KVLIST vector<pair<BinaryData,BinaryData> > 

#define DEFAULT_LDB_BLOCK_SIZE 32*1024

// Use this to create iterators that are intended for bulk scanning
// It's actually that the ReadOptions::fill_cache arg needs to be false
#define BULK_SCAN false

class BlockHeader;
class Tx;
class TxIn;
class TxOut;
class TxRef;
class TxIOPair;
class GlobalDBUtilities;

class StoredHeader;
class StoredTx;
class StoredTxOut;
class StoredScriptHistory;


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
// NOTE:  VERY IMPORTANT NOTE ABOUT THE DATABASE STRUCTURE
//
//    Almost everywhere you see integers serialized throughout Bitcoin, is uses
//    little-endian.  This is critical to follow because you are always handling
//    hashes of these serializations, so the byte-ordering matters.
//
// *HOWEVER*:  
//     
//    This database design relies on the natural ordering of database
//    keys which are frequently concatenations of integers.  For instance, each 
//    block is indexed by height, and we expect an iteration over all keys will
//    traverse the blocks in height-order.  BUT THIS DOESN'T WORK IF THE KEYS
//    ARE WRITTEN IN LITTLE-ENDIAN.  Therefore, all serialized integers in 
//    database KEYS are BIG-ENDIAN.  All other serializations in database VALUES
//    are LITTLE-ENDIAN (including var_ints, and all put/get_uintX_t() calls).
//
// *HOWEVER-2*:
//
//    This gets exceptionally confusing because some of the DB VALUES include 
//    references to DB KEYS, thus requiring those specific serializations to be 
//    BE, even though the rest of the data uses LE.
//
// REPEATED:
//
//    Database Keys:    BIG-ENDIAN integers
//    Database Values:  LITTLE-ENDIAN integers
//
//
// How to avoid getting yourself in a mess with this:
//
//    Always use hgtx methods:
//       hgtxToHeight( BinaryData(4) )
//       hgtxToDupID( BinaryData(4) )
//       heightAndDupToHgtx( uint32_t, uint8_t)
//
//    Always use BIGENDIAN for txIndex_ or txOutIndex_ serializations:
//       BinaryWriter.put_uint16_t(txIndex,    BIGENDIAN);
//       BinaryWriter.put_uint16_t(txOutIndex, BIGENDIAN);
//       BinaryReader.get_uint16_t(BIGENDIAN);
//       BinaryReader.get_uint16_t(BIGENDIAN);
//
//
// *OR*  
//
//    Don't mess with the internals of the DB!  The public methods that are
//    used to access the data in the DB externally do not require an 
//    understanding of how the data is actually serialized under the hood.
//
//
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

class LDBIter
{
public: 

   // fill_cache argument should be false for large bulk scans
   LDBIter(void) { isDirty_=true;}
   LDBIter(LMDB::Iterator&& move);
   LDBIter(LDBIter&& move);
   LDBIter(const LDBIter& cp);
   LDBIter& operator=(LMDB::Iterator&& move);
   LDBIter& operator=(LDBIter&& move);

   bool isNull(void) { return !iter_.isValid(); }
   bool isValid(void) { return iter_.isValid(); }
   bool isValid(DB_PREFIX dbpref);

   bool readIterData(void);
   
   bool retreat();
   bool advance(void);
   bool advance(DB_PREFIX prefix);
   bool advanceAndRead(void);
   bool advanceAndRead(DB_PREFIX prefix);

   BinaryData       getKey(void) const;
   BinaryData       getValue(void) const;
   BinaryDataRef    getKeyRef(void) const;
   BinaryDataRef    getValueRef(void) const;
   BinaryRefReader& getKeyReader(void) const;
   BinaryRefReader& getValueReader(void) const;

   // All the seekTo* methods do the exact same thing, the variant simply 
   // determines the meaning of the return true/false value.
   bool seekTo(BinaryDataRef key);
   bool seekTo(DB_PREFIX pref, BinaryDataRef key);
   bool seekToExact(BinaryDataRef key);
   bool seekToExact(DB_PREFIX pref, BinaryDataRef key);
   bool seekToStartsWith(BinaryDataRef key);
   bool seekToStartsWith(DB_PREFIX prefix);
   bool seekToStartsWith(DB_PREFIX pref, BinaryDataRef key);
   bool seekToBefore(BinaryDataRef key);
   bool seekToBefore(DB_PREFIX prefix);
   bool seekToBefore(DB_PREFIX pref, BinaryDataRef key);
   bool seekToFirst(void);

   // Return true if the iterator is currently on valid data, with key match
   bool checkKeyExact(BinaryDataRef key);
   bool checkKeyExact(DB_PREFIX prefix, BinaryDataRef key);
   bool checkKeyStartsWith(BinaryDataRef key);
   bool checkKeyStartsWith(DB_PREFIX prefix, BinaryDataRef key);

   bool verifyPrefix(DB_PREFIX prefix, bool advanceReader=true);

   void resetReaders(void){currKeyReader_.resetPosition();currValueReader_.resetPosition();}

private:

   LMDB::Iterator iter_;

   mutable BinaryData       currKey_;
   mutable BinaryData       currValue_;
   mutable BinaryRefReader  currKeyReader_;
   mutable BinaryRefReader  currValueReader_;
   bool isDirty_;
   
   
};


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
//
// InterfaceToLDB
//
// This is intended to be the only part of the project that communicates 
// directly with LevelDB objects.  All the public methods only interact with 
// BinaryData, BinaryDataRef, and BinaryRefReader objects.  
//
// As of this writing (the first implementation of this interface), the 
// interface and underlying DB structure is designed and tested for 
// ARMORY_DB_FULL and DB_PRUNE_NONE, which is essentially the same mode that 
// Armory used before the persistent blockchain upgrades.  However, much of
// the design decisions about how to store and access data was done to best
// accommodate future implementation of pruned/lite modes, as well as a 
// supernode mode (which tracks all addresses and can be used to respond to
// balance/UTXO queries from other nodes).  There is still implementation 
// work to be done to enable these other modes, but it shouldn't require
// changing the DB structure dramatically.  The biggest modification will 
// adding and tracking undo-data to deal with reorgs in a pruned blockchain.
//
// NOTE 1: This class was designed with certain optimizations that may cause 
//         unexpected behavior if you are not aware of it.  The following 
//         methods return/modify static member of InterfaceToLDB:
//
//            getValue*
//            seekTo*
//            start*Iteration
//            advance*
//
//         This is especially dangerous with getValueRef() which returns a
//         reference to lastGetValue_ which changes under you as soon as you
//         execute any other getValue* calls.  This eliminates unnecessary 
//         copying of DB data but can cause all sorts of problems if you are 
//         doing sequences of find-and-modify operations.  
//
//         It is best to avoid getValueRef() unless you are sure that you 
//         understand how to use it safely.  Only use getValue() unless there
//         is reason to believe that the optimization is needed.
//
//
//
// NOTE 2: Batch writing operations are smoothed so that multiple, nested
//         startBatch-commitBatch calls do not actually do anything except
//         at the outer-most level.  But this means that you MUST make sure
//         that there is a commit for every start, at every level.  If you
//         return a value from a method sidestepping a required commitBatch 
//         call, the code will stop writing to the DB at all!  
//          



////////////////////////////////////////////////////////////////////////////////
class LMDBBlockDatabase
{
public:

   /////////////////////////////////////////////////////////////////////////////
   LMDBBlockDatabase(function<bool(void)> isDBReady);
   ~LMDBBlockDatabase(void);

   /////////////////////////////////////////////////////////////////////////////
   void openDatabases(const string &basedir,
      BinaryData const & genesisBlkHash,
      BinaryData const & genesisTxHash,
      BinaryData const & magic,
      ARMORY_DB_TYPE     dbtype,
      DB_PRUNE_TYPE      pruneType);

   void openDatabasesSupernode(
      const string& basedir,
      BinaryData const & genesisBlkHash,
      BinaryData const & genesisTxHash,
      BinaryData const & magic,
      ARMORY_DB_TYPE     dbtype,
      DB_PRUNE_TYPE      pruneType);

   /////////////////////////////////////////////////////////////////////////////
   void nukeHeadersDB(void);

   /////////////////////////////////////////////////////////////////////////////
   void closeDatabases();
   void closeDatabasesSupernode(void);

   /////////////////////////////////////////////////////////////////////////////
   void beginDBTransaction(LMDBEnv::Transaction* tx, 
      DB_SELECT db, LMDB::Mode mode) const
   {
      if (armoryDbType_ == ARMORY_DB_SUPER)
         *tx = move(LMDBEnv::Transaction(dbEnv_[BLKDATA].get(), mode));
      else
         *tx = move(LMDBEnv::Transaction(dbEnv_[db].get(), mode));
   }

   ARMORY_DB_TYPE getDbType(void) const { return armoryDbType_; }

   DB_SELECT getDbSelect(DB_SELECT dbs) const
   {
      if (dbs == HEADERS)
         return HEADERS;

      if (armoryDbType_ == ARMORY_DB_SUPER)
         return BLKDATA;

      return dbs;
   }

   /////////////////////////////////////////////////////////////////////////////
   // Sometimes, we just need to nuke everything and start over
   void destroyAndResetDatabases(void);

   /////////////////////////////////////////////////////////////////////////////
   bool databasesAreOpen(void) { return dbIsOpen_; }

   /////////////////////////////////////////////////////////////////////////////
   // Get latest block info
   BinaryData getTopBlockHash(DB_SELECT db);
   uint32_t   getTopBlockHeight(DB_SELECT db);
   
   /////////////////////////////////////////////////////////////////////////////
   LDBIter getIterator(DB_SELECT db) const
   {
      return dbs_[db].begin();
   }


   /////////////////////////////////////////////////////////////////////////////
   // Get value using BinaryData object.  If you have a string, you can use
   // BinaryData key(string(theStr));
   BinaryData getValue(DB_SELECT db, BinaryDataRef keyWithPrefix) const;
   BinaryDataRef getValueNoCopy(DB_SELECT db, BinaryDataRef keyWithPrefix) const;
   
   /////////////////////////////////////////////////////////////////////////////
   // Get value using BinaryData object.  If you have a string, you can use
   // BinaryData key(string(theStr));
   BinaryData getValue(DB_SELECT db, DB_PREFIX pref, BinaryDataRef key) const;

   /////////////////////////////////////////////////////////////////////////////
   // Get value using BinaryDataRef object.  The data from the get* call is 
   // actually stored in a member variable, and thus the refs are valid only 
   // until the next get* call.
   BinaryDataRef getValueRef(DB_SELECT db, BinaryDataRef keyWithPrefix) const;
   BinaryDataRef getValueRef(DB_SELECT db, DB_PREFIX prefix, BinaryDataRef key) const;

   /////////////////////////////////////////////////////////////////////////////
   // Same as the getValueRef, in that they are only valid until the next get*
   // call.  These are convenience methods which basically just save us 
   BinaryRefReader getValueReader(DB_SELECT db, BinaryDataRef keyWithPrefix) const;
   BinaryRefReader getValueReader(DB_SELECT db, DB_PREFIX prefix, BinaryDataRef key) const;

   BinaryData getHashForDBKey(BinaryData dbkey);
   BinaryData getHashForDBKey(uint32_t hgt,
      uint8_t  dup,
      uint16_t txi = UINT16_MAX,
      uint16_t txo = UINT16_MAX);

   /////////////////////////////////////////////////////////////////////////////
   // Put value based on BinaryDataRefs key and value
   void putValue(DB_SELECT db, BinaryDataRef key, BinaryDataRef value);
   void putValue(DB_SELECT db, BinaryData const & key, BinaryData const & value);
   void putValue(DB_SELECT db, DB_PREFIX pref, BinaryDataRef key, BinaryDataRef value);

   /////////////////////////////////////////////////////////////////////////////
   // Put value based on BinaryData key.  If batch writing, pass in the batch
   void deleteValue(DB_SELECT db, BinaryDataRef key);
   void deleteValue(DB_SELECT db, DB_PREFIX pref, BinaryDataRef key);
   
   // Move the iterator in DB to the lowest entry with key >= inputKey
   bool seekTo(DB_SELECT db,
      BinaryDataRef key);
   bool seekTo(DB_SELECT db,
      DB_PREFIX pref,
      BinaryDataRef key);

   // Move the iterator to the first entry >= txHash
   bool seekToTxByHash(LDBIter & ldbIter, BinaryDataRef txHash) const;


   /////////////////////////////////////////////////////////////////////////////
   // "Skip" refers to the behavior that the previous operation may have left
   // the iterator already on the next desired block.  So our "advance" op may
   // have finished before it started.  Alternatively, we may be on this block 
   // because we checked it and decide we don't care, so we want to skip it.
   bool advanceToNextBlock(LDBIter & iter, bool skip = false) const;
   bool advanceIterAndRead(DB_SELECT, DB_PREFIX);

   bool dbIterIsValid(DB_SELECT db, DB_PREFIX prefix = DB_PREFIX_COUNT);

   /////////////////////////////////////////////////////////////////////////////
   void readAllHeaders(
      const function<void(const BlockHeader&, uint32_t, uint8_t)> &callback
   );

   /////////////////////////////////////////////////////////////////////////////
   // When we're not in supernode mode, we're going to need to track only 
   // specific addresses.  We will keep a list of those addresses here.
   // UINT32_MAX for the "scannedUpToBlk" arg means that this address is totally
   // new and does not require a rescan.  If you don't know when the scraddress
   // was created, use 0.  Or if you know something, you can supply it.  Though
   // in many cases, we will just do a full rescan if it's not totally new.
   void addRegisteredScript(BinaryDataRef rawScript,
      uint32_t      scannedUpToBlk = UINT32_MAX);



   /////////////////////////////////////////////////////////////////////////////
   bool startBlkDataIteration(LDBIter & iter, DB_PREFIX prefix);


   /////////////////////////////////////////////////////////////////////////////
   void getNextBlock(void);

   /////////////////////////////////////////////////////////////////////////////
   void getBlock(BlockHeader & bh,
      vector<Tx> & txList,
      LMDB::Iterator* iter = NULL,
      bool ignoreMerkle = true);


   /////////////////////////////////////////////////////////////////////////////
   void loadAllStoredHistory(void);

   map<HashString, BlockHeader> getHeaderMap(void);
   BinaryData getRawHeader(BinaryData const & headerHash);
   //bool addHeader(BinaryData const & headerHash, BinaryData const & headerRaw);

   map<uint32_t, uint32_t> getSSHSummary(BinaryDataRef scrAddrStr,
      uint32_t endBlock);

   uint32_t getStxoCountForTx(const BinaryData & dbKey6) const;

public:

   uint8_t getValidDupIDForHeight(uint32_t blockHgt) const;
   void setValidDupIDForHeight(uint32_t blockHgt, uint8_t dup,
      bool overwrite = true);

   /////////////////////////////////////////////////////////////////////////////
   uint8_t getValidDupIDForHeight_fromDB(uint32_t blockHgt);

   ////////////////////////////////////////////////////////////////////////////
   uint8_t getDupForBlockHash(BinaryDataRef blockHash);



   /////////////////////////////////////////////////////////////////////////////
   // Interface to translate Stored* objects to/from persistent DB storage
   /////////////////////////////////////////////////////////////////////////////
   void putStoredDBInfo(DB_SELECT db, StoredDBInfo const & sdbi);
   bool getStoredDBInfo(DB_SELECT db, StoredDBInfo & sdbi, bool warn = true);

   /////////////////////////////////////////////////////////////////////////////
   // BareHeaders are those int the HEADERS DB with no blockdta associated
   uint8_t putBareHeader(StoredHeader & sbh, bool updateDupID = true);
   bool    getBareHeader(StoredHeader & sbh, uint32_t blkHgt, uint8_t dup);
   bool    getBareHeader(StoredHeader & sbh, uint32_t blkHgt);
   bool    getBareHeader(StoredHeader & sbh, BinaryDataRef headHash);

   /////////////////////////////////////////////////////////////////////////////
   // StoredHeader accessors
   //For Supernode
   uint8_t putStoredHeader(StoredHeader & sbh,
      bool withBlkData = true,
      bool updateDupID = true);

   //for Fullnode
   uint8_t putRawBlockData(BinaryRefReader& brr, 
      function<const BlockHeader& (const BinaryData&)>);

   //getStoredHeader detects the dbType and update the passed StoredHeader
   //accordingly
   bool getStoredHeader(StoredHeader & sbh,
      uint32_t blockHgt,
      uint8_t blockDup = UINT8_MAX,
      bool withTx = true) const;

   bool getStoredHeader(StoredHeader & sbh,
      BinaryDataRef headHash,
      bool withTx = true) const;

   // This seemed unnecessary and was also causing conflicts with optional args
   //bool getStoredHeader(StoredHeader & sbh,
   //uint32_t blockHgt,
   //bool withTx=true);


   /////////////////////////////////////////////////////////////////////////////
   // StoredTx Accessors
   void updateStoredTx(StoredTx & st);

   void putStoredTx(StoredTx & st, bool withTxOut = true);
   void putStoredZC(StoredTx & stx, const BinaryData& zcKey);

   bool getStoredZcTx(StoredTx & stx,
      BinaryDataRef dbKey) const;

   bool getStoredTx(StoredTx & stx,
      BinaryData& txHashOrDBKey) const;

   bool getStoredTx_byDBKey(StoredTx & stx,
      BinaryDataRef dbKey) const;

   bool getStoredTx_byHash(const BinaryData& txHash,
      StoredTx* stx = nullptr,
      BinaryData* DBkey = nullptr) const;
   bool getStoredTx_byHashSuper(const BinaryData& txHash,
      StoredTx* stx = nullptr,
      BinaryData* DBkey = nullptr) const;

   bool getStoredTx(StoredTx & st,
      uint32_t blkHgt,
      uint16_t txIndex,
      bool withTxOut = true) const;

   bool getStoredTx(StoredTx & st,
      uint32_t blkHgt,
      uint8_t  dupID,
      uint16_t txIndex,
      bool withTxOut = true) const;


   /////////////////////////////////////////////////////////////////////////////
   // StoredTxOut Accessors
   void putStoredTxOut(StoredTxOut const & sto);
   void putStoredZcTxOut(StoredTxOut const & stxo, const BinaryData& zcKey);

   bool getStoredTxOut(StoredTxOut & stxo,
      uint32_t blockHeight,
      uint8_t  dupID,
      uint16_t txIndex,
      uint16_t txOutIndex) const;

   bool getStoredTxOut(StoredTxOut & stxo,
      uint32_t blockHeight,
      uint16_t txIndex,
      uint16_t txOutIndex) const;

   bool getStoredTxOut(StoredTxOut & stxo,
      const BinaryData& DBkey) const;

   void putStoredScriptHistory(StoredScriptHistory & ssh);
   void putStoredScriptHistorySummary(StoredScriptHistory & ssh);
   void putStoredSubHistory(StoredSubHistory & subssh);

   bool getStoredScriptHistory(StoredScriptHistory & ssh,
      BinaryDataRef scrAddrStr,
      uint32_t startBlock = 0,
      uint32_t endBlock = UINT32_MAX) const;

   bool getStoredSubHistoryAtHgtX(StoredSubHistory& subssh,
      const BinaryData& scrAddrStr, const BinaryData& hgtX) const;

   void getStoredScriptHistorySummary(StoredScriptHistory & ssh,
      BinaryDataRef scrAddrStr) const;

   void getStoredScriptHistoryByRawScript(
      StoredScriptHistory & ssh,
      BinaryDataRef rawScript) const;

   // This method breaks from the convention I've used for getting/putting 
   // stored objects, because we never really handle Sub-SSH objects directly,
   // but we do need to harness them.  This method could be renamed to
   // "getPartialScriptHistory()" ... it reads the main 
   // sub-SSH from DB and adds it to the supplied regular-SSH.
   bool fetchStoredSubHistory(StoredScriptHistory & ssh,
      BinaryData hgtX,
      bool createIfDNE = false,
      bool forceReadAndMerge = false);

   // This could go in StoredBlockObj if it didn't need to lookup DB data
   bool     getFullUTXOMapForSSH(StoredScriptHistory & ssh,
      map<BinaryData, UnspentTxOut> & mapToFill,
      bool withMultisig = false);

   uint64_t getBalanceForScrAddr(BinaryDataRef scrAddr, bool withMulti = false);

   // TODO: We should probably implement some kind of method for accessing or 
   //       running calculations on an SSH without ever loading the entire
   //       thing into RAM.  

   // None of the SUD methods are implemented because we don't actually need
   // to read/write SUD to the database -- our only mode is ARMORY_DB_SUPER
   // which doesn't require storing undo data
   bool putStoredUndoData(StoredUndoData const & sud);
   bool getStoredUndoData(StoredUndoData & sud, uint32_t height);
   bool getStoredUndoData(StoredUndoData & sud, uint32_t height, uint8_t dup);
   bool getStoredUndoData(StoredUndoData & sud, BinaryDataRef headHash);

   bool putStoredTxHints(StoredTxHints const & sths);
   bool getStoredTxHints(StoredTxHints & sths, BinaryDataRef hashPrefix);
   void updatePreferredTxHint(BinaryDataRef hashOrPrefix, BinaryData preferKey);

   bool putStoredHeadHgtList(StoredHeadHgtList const & hhl);
   bool getStoredHeadHgtList(StoredHeadHgtList & hhl, uint32_t height);

   ////////////////////////////////////////////////////////////////////////////
   // Some methods to grab data at the current iterator location.  Return
   // false if reading fails (maybe because we were expecting to find the
   // specified DB entry type, but the prefix byte indicated something else
   bool readStoredBlockAtIter(LDBIter & ldbIter, DBBlock & sbh) const;

   bool readStoredTxAtIter(LDBIter & iter,
      uint32_t height,
      uint8_t dupID,
      DBTx & stx) const;

   bool readStoredTxOutAtIter(LDBIter & iter,
      uint32_t height,
      uint8_t  dupID,
      uint16_t txIndex,
      StoredTxOut & stxo) const;

   bool readStoredScriptHistoryAtIter(LDBIter & iter,
      StoredScriptHistory & ssh,
      uint32_t startBlock,
      uint32_t endBlock) const;

   // TxRefs are much simpler with LDB than the previous FileDataPtr construct
   TxRef getTxRef(BinaryDataRef txHash);
   TxRef getTxRef(BinaryData hgtx, uint16_t txIndex);
   TxRef getTxRef(uint32_t hgt, uint8_t dup, uint16_t txIndex);


   // Sometimes we already know where the Tx is, but we don't know its hash
   Tx    getFullTxCopy(BinaryData ldbKey6B) const;
   Tx    getFullTxCopy(uint32_t hgt, uint16_t txIndex) const;
   Tx    getFullTxCopy(uint32_t hgt, uint8_t dup, uint16_t txIndex) const;
   TxOut getTxOutCopy(BinaryData ldbKey6B, uint16_t txOutIdx) const;
   TxIn  getTxInCopy(BinaryData ldbKey6B, uint16_t txInIdx) const;


   // Sometimes we already know where the Tx is, but we don't know its hash
   BinaryData getTxHashForLdbKey(BinaryDataRef ldbKey6B) const;

   BinaryData getTxHashForHeightAndIndex(uint32_t height,
      uint16_t txIndex);

   BinaryData getTxHashForHeightAndIndex(uint32_t height,
      uint8_t  dup,
      uint16_t txIndex);

   StoredTxHints getHintsForTxHash(BinaryDataRef txHash) const;


   ////////////////////////////////////////////////////////////////////////////
   bool markBlockHeaderValid(BinaryDataRef headHash);
   bool markBlockHeaderValid(uint32_t height, uint8_t dup);
   bool markTxEntryValid(uint32_t height, uint8_t dupID, uint16_t txIndex);


   /////////////////////////////////////////////////////////////////////////////
   void computeUndoDataFromRawBlock(StoredHeader const & sbh,
      StoredUndoData & sud);
   void computeUndoDataFromRawBlock(BinaryDataRef    rawBlock,
      StoredUndoData & sud);
   bool computeUndoDataForBlock(uint32_t height,
      uint8_t dupID,
      StoredUndoData & sud);


   KVLIST getAllDatabaseEntries(DB_SELECT db);
   void   printAllDatabaseEntries(DB_SELECT db);
   void   pprintBlkDataDB(uint32_t indent = 3);

   BinaryData getGenesisBlockHash(void) { return genesisBlkHash_; }
   BinaryData getGenesisTxHash(void)    { return genesisTxHash_; }
   BinaryData getMagicBytes(void)       { return magicBytes_; }

   bool isReady(void) { return isDBReady_(); }
   ARMORY_DB_TYPE armoryDbType(void) { return armoryDbType_; }

private:
   string               baseDir_;
   string dbBlkdataFilename() const { return baseDir_ + "/blocks";  }
   string dbHeadersFilename() const { return baseDir_ + "/headers"; }
   string dbHistoryFilename() const { return baseDir_ + "/history"; }
   string dbTxhintsFilename() const { return baseDir_ + "/txhints"; }

   BinaryData           genesisBlkHash_;
   BinaryData           genesisTxHash_;
   BinaryData           magicBytes_;

   ARMORY_DB_TYPE armoryDbType_;
   DB_PRUNE_TYPE dbPruneType_;

public:

   mutable map<DB_SELECT, shared_ptr<LMDBEnv> > dbEnv_;
   mutable LMDB dbs_[COUNT];

private:
   //leveldb::FilterPolicy* dbFilterPolicy_[2];

   //BinaryRefReader      currReadKey_;
   //BinaryRefReader      currReadValue_;;
   //mutable BinaryData           lastGetValue_;

   bool                 dbIsOpen_;
   uint32_t             ldbBlockSize_;

   uint32_t             lowestScannedUpTo_;

   map<uint32_t, uint8_t>      validDupByHeight_;

   // In this case, a address is any TxOut script, which is usually
   // just a 25-byte script.  But this generically captures all types
   // of addresses including pubkey-only, P2SH, 
   map<BinaryData, StoredScriptHistory>   registeredSSHs_;

   const BinaryData ZCprefix_ = BinaryData(2);

   function<bool(void)> isDBReady_ = [](void)->bool{ return false; };
};

#endif
// kate: indent-width 3; replace-tabs on;
