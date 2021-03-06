// MegaHash v1.0
// Copyright (c) 2019 Joseph Huckaby
// Based on DeepHash, (c) 2003 Joseph Huckaby

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define MIN(a,b) (((a)<(b))?(a):(b))
#define MAX(a,b) (((a)>(b))?(a):(b))

/** Type used for key lengths. */
#define BH_KLEN_T uint16_t
/** Type used for value lengths. */
#define BH_LEN_T uint32_t

/** Length of BH_KLEN_T and BH_LEN_T. */
#define BH_KLEN_SIZE sizeof(BH_KLEN_T)
#define BH_LEN_SIZE sizeof(BH_LEN_T)

/** Size of one hashed key, in bytes. */
#define BH_DIGEST_SIZE 8

/** Size of one index level. */
#define BH_INDEX_SIZE 16

/** \name Result codes after pair is stored or fetched:
	These all go into the result property of the Response object. */
//@{
/** Error occured during operation. */
#define BH_ERR 0
/** Result was okay (used only in fetch()). */
#define BH_OK 1
/** Result was add (key was unique). */
#define BH_ADD 1
/** Result was replace (key existed and value was overwritten). */
#define BH_REPLACE 2
//@}

/** \name Signatures used to identify tags: */
//@{
/** Signature used for identifying index tags. */
#define BH_SIG_INDEX 'I'
/** Signature used for identifying bucket tags. */
#define BH_SIG_BUCKET 'B'
//@}

class Stats {
public:
	// current stats about the hash table
	uint64_t numKeys;
	uint64_t indexSize;
	uint64_t metaSize;
	uint64_t dataSize;
	
	Stats() {
		numKeys = 0;
		indexSize = 0;
		metaSize = 0;
		dataSize = 0;
	}
};

class Response {
public:
	// a response object is returned from all hash table operations
	unsigned char result;
	unsigned char flags;
	unsigned char *content; /**< Pointer to content.  Can be binary or string. */
	BH_LEN_T contentLength; /**< Length of content. */
	
	Response() {
		content = NULL;
		contentLength = 0;
		result = 0;
		flags = 0;
	}
};

class Tag {
public:
	// a tag is a base class shared by indexes and buckets
	unsigned char type;
};

class Index : public Tag {
public:
	// an index represents 4 bits of the key hash, and has 16 slots
	// each slot may point to another index, or a bucket linked list
	Tag *data[BH_INDEX_SIZE];
	
	Index() {
		type = BH_SIG_INDEX;
		for (int idx = 0; idx < BH_INDEX_SIZE; idx++) data[idx] = NULL;
	}
};

class Bucket : public Tag {
public:
	// a bucket represents one key/value pair in the hash table
	// this is also a linked list, for collisions
	// currently this is 24 bytes
	unsigned char flags;
	unsigned char *data;
	Bucket *next;
	
	Bucket() {
		type = BH_SIG_BUCKET;
		flags = 0;
		data = NULL;
		next = NULL;
	}
};

class Hash {
public:
	// main hash table object
	// starts with one 8-bit index (auto-expands)
	Index *index;
	Stats *stats;
	unsigned char maxBuckets;
	unsigned char reindexScatter;
	
	Hash() {
		maxBuckets = 16;
		reindexScatter = 1;
		init();
	}
	
	Hash(unsigned char newMaxBuckets) {
		maxBuckets = newMaxBuckets;
		if (maxBuckets < 1) maxBuckets = 1;
		reindexScatter = 1;
		init();
	}
	
	Hash(unsigned char newMaxBuckets, unsigned char newReindexScatter) {
		maxBuckets = newMaxBuckets;
		if (maxBuckets < 1) maxBuckets = 1;
		
		reindexScatter = newReindexScatter;
		if (reindexScatter < 1) reindexScatter = 1;
		if ((int)maxBuckets + (int)reindexScatter > 256) reindexScatter = 1;
		
		init();
	}
	
	~Hash() {
		clear();
	}
	
	void init() {
		index = new Index();
		stats = new Stats();
		stats->indexSize += sizeof(Index);
	}
	
	// public methods:
	Response store(unsigned char *key, BH_KLEN_T keyLength, unsigned char *content, BH_LEN_T contentLength, unsigned char flags = 0);
	Response fetch(unsigned char *key, BH_KLEN_T keyLength);
	Response remove(unsigned char *key, BH_KLEN_T keyLength);
	Response firstKey();
	Response nextKey(unsigned char *key, BH_KLEN_T keyLength);
	
	void clear();
	void clear(unsigned char slice);
	
	// internal methods:
	void clearTag(Tag *tag);
	void reindexBucket(Bucket *bucket, Index *index, unsigned char digestIndex);
	void traverseTag(Response *resp, Tag *tag, unsigned char *key, BH_KLEN_T keyLength, unsigned char *digest, unsigned char digestIndex, unsigned char *returnNext);
	
	int bucketKeyEquals(unsigned char *bucketData, unsigned char *key, BH_KLEN_T keyLength) {
		// compare key to bucket key
		BH_KLEN_T bucketKeyLength = bucketGetKeyLength(bucketData);
		unsigned char *bucketKey = bucketData + BH_KLEN_SIZE;
		if (keyLength != bucketKeyLength) return 0;
		return (int)!memcmp( (void *)key, (void *)bucketKey, (size_t)keyLength );
	}
	
	BH_KLEN_T bucketGetKeyLength(unsigned char *bucketData) {
		// get bucket key length
		BH_KLEN_T *tempKL = (BH_KLEN_T *)bucketData;
		return tempKL[0];
	}
	
	unsigned char *bucketGetKey(unsigned char *bucketData) {
		// get pointer to bucket key
		return bucketData + BH_KLEN_SIZE;
	}
	
	BH_LEN_T bucketGetContentLength(unsigned char *bucketData) {
		// get bucket content (value) length
		BH_KLEN_T keyLength = bucketGetKeyLength(bucketData);
		unsigned char *tempCL = bucketData + BH_KLEN_SIZE + keyLength;
		return ((BH_LEN_T *)tempCL)[0];
	}
	
	unsigned char *bucketGetContent(unsigned char *bucketData) {
		// get pointer to bucket content (value)
		BH_KLEN_T keyLength = bucketGetKeyLength(bucketData);
		return bucketData + BH_KLEN_SIZE + keyLength + BH_LEN_SIZE;
	}
	
	void digestKey(unsigned char *key, BH_KLEN_T keyLength, unsigned char *digest) {
		// Create 32-bit digest of custom key using DJB2 algorithm.
		// Return as 8 separate bytes (4 bits each) in unsigned char array
		uint32_t hash = 5381;
		for (unsigned int i = 0; i < keyLength; i++) {
			hash = ((hash << 5) + hash) + key[i];
		}
		((uint32_t *)digest)[0] = hash;
		
		digest[4] = digest[0] % 16;
		digest[5] = digest[1] % 16;
		digest[6] = digest[2] % 16;
		digest[7] = digest[3] % 16;
		
		digest[0] /= 16;
		digest[1] /= 16;
		digest[2] /= 16;
		digest[3] /= 16;
	}

}; // Hash
