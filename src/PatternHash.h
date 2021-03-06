#ifndef _PATTERNHASH_H_
#define _PATTERNHASH_H_

#include "GoBoard.h"
#include "Pattern.h"

const int HASH_MAX = 1048576; // 2^20
const int BIT_MAX = 60;

#define TRANS20(hash)	(((hash&0xFFFFFFFF)^((hash>>32)&0xFFFFFFFF))&0xFFFFF)

// パターン
struct pattern_hash_t {
  unsigned long long list[MD_MAX + MD_LARGE_MAX];
};

// インデックス 
struct index_hash_t {
  unsigned long long hash;
  int index;
};

////////////
//  関数  //
////////////

//  パターンのハッシュ関数
void PatternHash( const pattern *pat, pattern_hash_t *hash_pat );

//  パターンのハッシュ関数
unsigned long long MD2Hash( unsigned int md2 );
unsigned long long MD3Hash( unsigned int md3 );
unsigned long long MD4Hash( unsigned int md4 );
unsigned long long MD5Hash( unsigned long long int md5 );

//  インデックスを探索
int SearchIndex( index_hash_t *index, unsigned long long hash );

#endif	// _PATTTERNHASH_H_ 
