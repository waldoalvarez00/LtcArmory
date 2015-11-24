////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2011-2015, Armory Technologies, Inc.                        //
//  Distributed under the GNU Affero General Public License (AGPL v3)         //
//  See LICENSE or http://www.gnu.org/licenses/agpl.html                      //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////
#ifndef BLOCKDATAMANAGERCONFIG_H
#define BLOCKDATAMANAGERCONFIG_H

#include "BinaryData.h"

enum ARMORY_DB_TYPE
{
  ARMORY_DB_BARE, // only raw block data
  ARMORY_DB_LITE,
  ARMORY_DB_PARTIAL,
  ARMORY_DB_FULL,
  ARMORY_DB_SUPER,
  ARMORY_DB_WHATEVER
};

enum DB_PRUNE_TYPE
{
  DB_PRUNE_ALL,
  DB_PRUNE_NONE,
  DB_PRUNE_WHATEVER
};


struct BlockDataManagerConfig
{
   ARMORY_DB_TYPE armoryDbType;
   DB_PRUNE_TYPE pruneType;
   
   string blkFileLocation;
   string levelDBLocation;
   
   BinaryData genesisBlockHash;
   BinaryData genesisTxHash;
   BinaryData magicBytes;
   
   void setGenesisBlockHash(const BinaryData &h)
   {
      genesisBlockHash = h;
   }
   void setGenesisTxHash(const BinaryData &h)
   {
      genesisTxHash = h;
   }
   void setMagicBytes(const BinaryData &h)
   {
      magicBytes = h;
   }
   
   BlockDataManagerConfig();
   BlockDataManagerConfig(const BlockDataManagerConfig& in);
   BlockDataManagerConfig& operator=(const BlockDataManagerConfig& in);
   void selectNetwork(const string &netname);
};

#endif
// kate: indent-width 3; replace-tabs on;
