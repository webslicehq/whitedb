/*
* $Id:  $
* $Version: $
*
* Copyright (c) Tanel Tammet 2004,2005,2006,2007,2008,2009
* Copyright (c) Priit J�rv 2013
*
* Contact: tanel.tammet@gmail.com                 
*
* This file is part of wgandalf
*
* Wgandalf is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
* 
* Wgandalf is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
* 
* You should have received a copy of the GNU General Public License
* along with Wgandalf.  If not, see <http://www.gnu.org/licenses/>.
*
*/

 /** @file dbhash.c
 *  Hash operations for strings and other datatypes. 
 *  
 *
 */

/* ====== Includes =============== */


#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
#include "../config-w32.h"
#else
#include "../config.h"
#endif
#include "dbhash.h"
#include "dbdata.h"


/* ====== Private headers and defs ======== */

/* Bucket capacity > 1 reduces the impact of freak collisions */
#define GINTHASH_BUCKETCAP 3

/* Level 24 hash consumes approx 640MB with bucket capacity 3 on 32-bit
 * architecture and about twice as much on 64-bit systems.
 */
#define GINTHASH_MAXLEVEL 24

/* rehash keys (useful for lowering the impact of bad distribution) */
#define GINTHASH_SCRAMBLE(v) (rehash_gint(v))
/*#define GINTHASH_SCRAMBLE(v) (v)*/

typedef struct {
  gint level;                         /* local level */
  gint fill;                          /* slots filled / next slot index */
  gint key[GINTHASH_BUCKETCAP + 1];   /* includes one overflow slot */
  gint value[GINTHASH_BUCKETCAP + 1];
} ginthash_bucket;

/* Dynamic local memory hashtable for gint key/value pairs. Resize
 * is handled using the extendible hashing algorithm.
 * Note: we don't use 0-level hash, so buckets[0] is unused.
 */
typedef struct {
  gint level;                  /* global level */
  ginthash_bucket **directory; /* bucket pointers, contiguous memory */
  ginthash_bucket *buckets[GINTHASH_MAXLEVEL];   /* bucket storage, allocated
                                                  * as needed */
  size_t nextbucket[GINTHASH_MAXLEVEL];          /* free bucket index */
  gint free_pool;              /* the first bucket pool with free space */
} ext_ginthash;

/* ======= Private protos ================ */



// static gint show_consistency_error(void* db, char* errmsg);
static gint show_consistency_error_nr(void* db, char* errmsg, gint nr) ;
// static gint show_consistency_error_double(void* db, char* errmsg, double nr);
// static gint show_consistency_error_str(void* db, char* errmsg, char* str);
static gint show_ginthash_error(char* errmsg);

static gint rehash_gint(gint val);
static gint grow_ginthash(ext_ginthash *tbl);
static ginthash_bucket *ginthash_newbucket(ext_ginthash *tbl);
static ginthash_bucket *ginthash_splitbucket(ext_ginthash *tbl,
  ginthash_bucket *bucket);
static gint add_to_bucket(ginthash_bucket *bucket, gint key, gint value);
static gint remove_from_bucket(ginthash_bucket *bucket, int idx);

/* ====== Functions ============== */


/* ------------- strhash operations ------------------- */




/* Hash function for two-part strings and blobs.
*
* Based on sdbm.
*
*/

int wg_hash_typedstr(void* db, char* data, char* extrastr, gint type, gint length) {
  char* endp;
  unsigned long hash = 0;
  int c;  
  
  //printf("in wg_hash_typedstr %s %s %d %d \n",data,extrastr,type,length);
  if (data!=NULL) {
    for(endp=data+length; data<endp; data++) {
      c = (int)(*data);
      hash = c + (hash << 6) + (hash << 16) - hash;
    }
  }  
  if (extrastr!=NULL) {
    while ((c = *extrastr++))
      hash = c + (hash << 6) + (hash << 16) - hash;    
  }  
  
  return (int)(hash % (dbmemsegh(db)->strhash_area_header).arraylength);
}



/* Find longstr from strhash bucket chain
*
*
*/

gint wg_find_strhash_bucket(void* db, char* data, char* extrastr, gint type, gint size, gint hashchain) {  
  //printf("wg_find_strhash_bucket called %s %s type %d size %d hashchain %d\n",data,extrastr,type,size,hashchain);
  for(;hashchain!=0;
      hashchain=dbfetch(db,decode_longstr_offset(hashchain)+LONGSTR_HASHCHAIN_POS*sizeof(gint))) {      
    if (wg_right_strhash_bucket(db,hashchain,data,extrastr,type,size)) {
      // found equal longstr, return it
      //printf("wg_find_strhash_bucket found hashchain %d\n",hashchain);
      return hashchain;
    }          
  }
  return 0;  
}

/* Check whether longstr hash bucket matches given new str
*
*
*/

int wg_right_strhash_bucket
            (void* db, gint longstr, char* cstr, char* cextrastr, gint ctype, gint cstrsize) {
  char* str;
  char* extrastr;
  int strsize;
  gint type;
  //printf("wg_right_strhash_bucket called with %s %s type %d size %d\n",
  //              cstr,cextrastr,ctype,cstrsize);
  type=wg_get_encoded_type(db,longstr);
  if (type!=ctype) return 0;
  strsize=wg_decode_str_len(db,longstr)+1;    
  if (strsize!=cstrsize) return 0;
  str=wg_decode_str(db,longstr); 
  if ((cstr==NULL && str!=NULL) || (cstr!=NULL && str==NULL)) return 0;             
  if ((cstr!=NULL) && (memcmp(str,cstr,cstrsize))) return 0;             
  extrastr=wg_decode_str_lang(db,longstr);
  if ((cextrastr==NULL && extrastr!=NULL) || (cextrastr!=NULL && extrastr==NULL)) return 0;
  if ((cextrastr!=NULL) && (strcmp(extrastr,cextrastr))) return 0; 
  return 1;
}  

/* Remove longstr from strhash
*
*  Internal langstr etc are not removed by this op.
*
*/

gint wg_remove_from_strhash(void* db, gint longstr) {
  db_memsegment_header* dbh = dbmemsegh(db);
  gint type;
  gint* extrastrptr;
  char* extrastr;
  char* data;
  gint length;
  gint hash;
  gint chainoffset;  
  gint hashchain;
  gint nextchain;
  gint offset;
  gint* objptr;
  gint fldval;
  gint objsize;
  gint strsize;
  gint* typeptr;
  
  //printf("wg_remove_from_strhash called on %d\n",longstr);  
  //wg_debug_print_value(db,longstr);
  //printf("\n\n");
  offset=decode_longstr_offset(longstr);
  objptr=(gint*) offsettoptr(db,offset);
  // get string data elements  
  //type=objptr=offsettoptr(db,decode_longstr_offset(data));       
  extrastrptr=(gint *) (((char*)(objptr))+(LONGSTR_EXTRASTR_POS*sizeof(gint)));
  fldval=*extrastrptr; 
  if (fldval==0) extrastr=NULL;
  else extrastr=wg_decode_str(db,fldval); 
  data=((char*)(objptr))+(LONGSTR_HEADER_GINTS*sizeof(gint));
  objsize=getusedobjectsize(*objptr);         
  strsize=objsize-(((*(objptr+LONGSTR_META_POS))&LONGSTR_META_LENDIFMASK)>>LONGSTR_META_LENDIFSHFT); 
  length=strsize;  
  typeptr=(gint*)(((char*)(objptr))+(+LONGSTR_META_POS*sizeof(gint)));
  type=(*typeptr)&LONGSTR_META_TYPEMASK;
  //type=wg_get_encoded_type(db,longstr);   
  // get hash of data elements and find the location in hashtable/chains   
  hash=wg_hash_typedstr(db,data,extrastr,type,length);  
  chainoffset=((dbh->strhash_area_header).arraystart)+(sizeof(gint)*hash);
  hashchain=dbfetch(db,chainoffset);    
  while(hashchain!=0) {
    if (hashchain==longstr) {
      nextchain=dbfetch(db,decode_longstr_offset(hashchain)+(LONGSTR_HASHCHAIN_POS*sizeof(gint)));  
      dbstore(db,chainoffset,nextchain);     
      return 0;  
    }        
    chainoffset=decode_longstr_offset(hashchain)+(LONGSTR_HASHCHAIN_POS*sizeof(gint));
    hashchain=dbfetch(db,chainoffset);
  }    
  show_consistency_error_nr(db,"string not found in hash during deletion, offset",offset);
  return -1;  
}


/* ------- local-memory extendible gint hash ---------- */

/*
 * Dynamically growing gint hash.
 *
 * Implemented in local memory for temporary usage (database memory is not well
 * suited as it is not resizable). Uses the extendible hashing algorithm
 * proposed by Fagin et al '79 as this allows the use of simple, easily
 * disposable data structures.
 */

/** Initialize the hash table.
 *  The initial hash level is 1.
 *  returns NULL on failure.
 */
void *wg_ginthash_init() {
  ext_ginthash *tbl = malloc(sizeof(ext_ginthash));
  if(!tbl) {
    show_ginthash_error("Failed to allocate table.");
    return NULL;
  }

  memset(tbl, 0, sizeof(ext_ginthash));
  if(grow_ginthash(tbl)) { /* initial level is set to 1 */
    free(tbl);
    return NULL;
  }
  tbl->free_pool = 1;
  return tbl;
}

/** Add a key/value pair to the hash table.
 *  tbl should be created with wg_ginthash_init()
 *  Returns 0 on success
 *  Returns -1 on failure
 */
gint wg_ginthash_addkey(void *tbl, gint key, gint val) {
  size_t dirsize = 1<<((ext_ginthash *)tbl)->level;
  size_t hash = GINTHASH_SCRAMBLE(key) & (dirsize - 1);
  ginthash_bucket *bucket = ((ext_ginthash *)tbl)->directory[hash];
  /*static gint keys = 0;*/
  /* printf("add: %d hash %d items %d\n", key, hash, ++keys); */
  if(!bucket) {
    /* allocate a new bucket, store value, we're done */
    bucket = ginthash_newbucket((ext_ginthash *) tbl);
    if(!bucket)
      return -1;
    bucket->level = ((ext_ginthash *) tbl)->level;
    add_to_bucket(bucket, key, val); /* Always fits, no check needed */
    ((ext_ginthash *)tbl)->directory[hash] = bucket;
  }
  else {
    add_to_bucket(bucket, key, val);
    while(bucket->fill > GINTHASH_BUCKETCAP) {
      ginthash_bucket *newb;
      /* Overflow, bucket split needed. */
      if(!(newb = ginthash_splitbucket((ext_ginthash *)tbl, bucket)))
        return -1;
      /* Did everything flow to the new bucket, causing another overflow? */
      if(newb->fill > GINTHASH_BUCKETCAP) {
        bucket = newb; /* Keep splitting */
      }
    }
  }
  return 0;
}

/** Fetch a value from the hash table.
 *  If the value is not found, returns -1 (val is unmodified).
 *  Otherwise returns 0; contents of val is replaced with the
 *  value from the hash table.
 */
gint wg_ginthash_getkey(void *tbl, gint key, gint *val) {
  size_t dirsize = 1<<((ext_ginthash *)tbl)->level;
  size_t hash = GINTHASH_SCRAMBLE(key) & (dirsize - 1);
  ginthash_bucket *bucket = ((ext_ginthash *)tbl)->directory[hash];
  if(bucket) {
    int i;
    for(i=0; i<bucket->fill; i++) {
      if(bucket->key[i] == key) {
        *val = bucket->value[i];
        return 0;
      }
    }
  }
  return -1;
}

/** Release all memory allocated for the hash table.
 *
 */
void wg_ginthash_free(void *tbl) {
  if(tbl) {
    int i;
    if(((ext_ginthash *) tbl)->directory)
      free(((ext_ginthash *) tbl)->directory);
    for(i=0; i<GINTHASH_MAXLEVEL; i++) {
      if(((ext_ginthash *) tbl)->buckets[i])
        free(((ext_ginthash *)tbl)->buckets[i]);
    }
    free(tbl);
  }
}

/** Scramble a gint value
 *  This is useful when dealing with aligned offsets, that are
 *  multiples of 4, 8 or larger values and thus waste the majority
 *  of the directory space when used directly.
 */
static gint rehash_gint(gint val) {
  int i;
  gint hash = 0;
  for(i=0; i<sizeof(gint); i++) {
    hash = ((char *) &val)[i] + (hash << 6) + (hash << 16) - hash;
  }
  return hash;
}

/** Grow the hash directory and allocate a new bucket pool.
 *
 */
static gint grow_ginthash(ext_ginthash *tbl) {
  void *tmp;
  gint newlevel = tbl->level + 1;
  if(newlevel >= GINTHASH_MAXLEVEL)
    return show_ginthash_error("Maximum level exceeded.");

  if((tmp = realloc((void *) tbl->directory,
    (1<<newlevel) * sizeof(ginthash_bucket *)))) {
    size_t nextpool_sz;
    tbl->directory = (ginthash_bucket **) tmp;

    if(tbl->level) {
      size_t i;
      nextpool_sz = 1<<tbl->level;      /* double the existing storage */
      /* duplicate the existing pointers. The size of the new bucket
       * pool is equal to the old directory size, so no need to compute
       * the latter separately.
       */
      for(i=0; i<nextpool_sz; i++)
        tbl->directory[nextpool_sz + i] = tbl->directory[i];
    } else {
      nextpool_sz = 2;             /* initial pool size (2 buckets at lv 1) */
      memset(tbl->directory, 0,
        2*sizeof(ginthash_bucket *));   /* initial directory is empty */
    }

    if((tmp = malloc(nextpool_sz * sizeof(ginthash_bucket)))) {
      tbl->buckets[newlevel] = tmp;
    } else {
      return show_ginthash_error("Failed to allocate bucket pool.");
    }
  } else {
    return show_ginthash_error("Failed to reallocate directory.");
  }
  tbl->level = newlevel;
  return 0;
}

/** Allocate a new bucket.
 *
 */
static ginthash_bucket *ginthash_newbucket(ext_ginthash *tbl) {
#ifdef CHECK
  if(tbl->free_pool > tbl->level) {
    /* Paranoia */
    show_ginthash_error("Demand for buckets too high (possible bug?).");
    return NULL;
  } else {
#endif
    gint pool = tbl->free_pool;
    size_t poolsz = (pool == 1 ? 2 : 1<<(pool-1));
    ginthash_bucket *bucket = &(tbl->buckets[pool][tbl->nextbucket[pool]++]);
    /* bucket->level = tbl->level; */
    bucket->fill = 0;
    if(tbl->nextbucket[pool] >= poolsz) {
      tbl->free_pool++;
    }
    return bucket;
#ifdef CHECK
  }
#endif
}

/** Split a bucket.
 *  Returns the newly created bucket on success
 *  Returns NULL on failure (likely cause being out of memory)
 */
static ginthash_bucket *ginthash_splitbucket(ext_ginthash *tbl,
  ginthash_bucket *bucket)
{
  gint msbmask, lowbits;
  int i;
  ginthash_bucket *newbucket;

  if(bucket->level == tbl->level) {
    /* can't split at this level anymore, extend directory */
    /*printf("grow: curr level %d\n", tbl->level);*/
    if(grow_ginthash((ext_ginthash *) tbl))
      return NULL;
  }

  /* Hash values for the new level (0+lowbits, msb+lowbits) */
  msbmask = (1<<(bucket->level++));
  lowbits = GINTHASH_SCRAMBLE(bucket->key[0]) & (msbmask - 1);

  /* Create a bucket to split into */
  newbucket = ginthash_newbucket(tbl);
  if(!newbucket)
    return NULL;
  newbucket->level = bucket->level;

  /* Split the entries based on the most significant
   * bit for the local level hash (the ones with msb set are relocated)
   */
  for(i=bucket->fill-1; i>=0; i--) {
    gint k_i = bucket->key[i];
    if(GINTHASH_SCRAMBLE(k_i) & msbmask) {
      add_to_bucket(newbucket, k_i, remove_from_bucket(bucket, i));
      /* printf("reassign: %d hash %d --> %d\n",
        k_i, lowbits, msbmask | lowbits); */
    }
  }

  /* Update the directory */
  if(bucket->level == tbl->level) {
    /* There are just two pointers, we can compute their location */
    /* tbl->directory[lowbits] = bucket; */ /* should already be there */
    tbl->directory[msbmask | lowbits] = newbucket;
  } else {
    /* 4 or more pointers, scan the directory */
    size_t dirsize = 1<<tbl->level, j;
    for(j=0; j<dirsize; j++) {
      if(tbl->directory[j] == bucket) {
        if(j & msbmask)
          tbl->directory[j] = newbucket;
      }
    }
  }
  return newbucket;
}

/** Add a key/value pair to bucket.
 *  Returns bucket fill.
 */
static gint add_to_bucket(ginthash_bucket *bucket, gint key, gint value) {
#ifdef CHECK
  if(bucket->fill > GINTHASH_BUCKETCAP) { /* Should never happen */
    show_ginthash_error("Out of overflow space, data dropped (FATAL)");
    return bucket->fill + 1;
  } else {
#endif
    bucket->key[bucket->fill] = key;
    bucket->value[bucket->fill] = value;
    return ++(bucket->fill);
#ifdef CHECK
  }
#endif
}

/** Remove an indexed value from bucket.
 *  Returns the value.
 */
static gint remove_from_bucket(ginthash_bucket *bucket, int idx) {
  int i;
  gint val = bucket->value[idx];
  for(i=idx; i<GINTHASH_BUCKETCAP; i++) {
    /* Note we ignore the last slot. Generally keys/values
     * in slots indexed >=bucket->fill are always undefined
     * and shouldn't be accessed directly.
     */
    bucket->key[i] = bucket->key[i+1];
    bucket->value[i] = bucket->value[i+1];
  }
  bucket->fill--;
  return val;
}

/* -------------    error handling  ------------------- */

/*

static gint show_consistency_error(void* db, char* errmsg) {
  printf("wg consistency error: %s\n",errmsg);
  return -1;
}
*/

static gint show_consistency_error_nr(void* db, char* errmsg, gint nr) {
  printf("wg consistency error: %s %d\n", errmsg, (int) nr);
  return -1;
}

/*
static gint show_consistency_error_double(void* db, char* errmsg, double nr) {
  printf("wg consistency error: %s %f\n",errmsg,nr);
  return -1;
}

static gint show_consistency_error_str(void* db, char* errmsg, char* str) {
  printf("wg consistency error: %s %s\n",errmsg,str);
  return -1;
}
*/

static gint show_ginthash_error(char* errmsg) {
  printf("wg gint hash error: %s\n", errmsg);
  return -1;
}

/*

#include "pstdint.h" // Replace with <stdint.h> if appropriate 
#undef get16bits
#if (defined(__GNUC__) && defined(__i386__)) || defined(__WATCOMC__) \
  || defined(_MSC_VER) || defined (__BORLANDC__) || defined (__TURBOC__)
#define get16bits(d) (*((const uint16_t *) (d)))
#endif

#if !defined (get16bits)
#define get16bits(d) ((((uint32_t)(((const uint8_t *)(d))[1])) << 8)\
                       +(uint32_t)(((const uint8_t *)(d))[0]) )
#endif

uint32_t SuperFastHash (const char * data, int len) {
uint32_t hash = len, tmp;
int rem;

    if (len <= 0 || data == NULL) return 0;

    rem = len & 3;
    len >>= 2;

    // Main loop 
    for (;len > 0; len--) {
        hash  += get16bits (data);
        tmp    = (get16bits (data+2) << 11) ^ hash;
        hash   = (hash << 16) ^ tmp;
        data  += 2*sizeof (uint16_t);
        hash  += hash >> 11;
    }

    // Handle end cases 
    switch (rem) {
        case 3: hash += get16bits (data);
                hash ^= hash << 16;
                hash ^= data[sizeof (uint16_t)] << 18;
                hash += hash >> 11;
                break;
        case 2: hash += get16bits (data);
                hash ^= hash << 11;
                hash += hash >> 17;
                break;
        case 1: hash += *data;
                hash ^= hash << 10;
                hash += hash >> 1;
    }

    // Force "avalanching" of final 127 bits 
    hash ^= hash << 3;
    hash += hash >> 5;
    hash ^= hash << 4;
    hash += hash >> 17;
    hash ^= hash << 25;
    hash += hash >> 6;

    return hash;
}

*/

#ifdef __cplusplus
}
#endif
