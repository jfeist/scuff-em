/*
 * FIPPICache.cc -- implementation of the FIPPICache class for libscuff
 * 
 * homer reid    -- 11/2005 -- 1/2011
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <tr1/unordered_map>

#include <libhrutil.h>

#include "libscuff.h"
#include "libscuffInternals.h"

namespace scuff {

#define KEYLEN 15

int Found;
int NotFound;

/*--------------------------------------------------------------*/
/*- note: i found this on wikipedia ... ------------------------*/
/*--------------------------------------------------------------*/
long JenkinsHash(char *key, size_t len)
{
    long hash; 
    int i;
    for(hash = i = 0; i < len; ++i)
    {
        hash += key[i];
        hash += (hash << 10);
        hash ^= (hash >> 6);
    }
    hash += (hash << 3);
    hash ^= (hash >> 11);
    hash += (hash << 15);
    return hash;
}

long HashFunction(const double *Key)
{ return JenkinsHash( (char *)Key, KEYLEN*sizeof(double) );
} 

/*--------------------------------------------------------------*/
/*--------------------------------------------------------------*/
/*--------------------------------------------------------------*/
typedef struct
 { double Key[KEYLEN];
 } KeyStruct;

typedef std::pair<KeyStruct, QIFIPPIData *> KeyValuePair;

struct KeyHash
 {
   long operator() (const KeyStruct &K) const { return HashFunction(K.Key); }
 };

typedef struct 
 { 
   bool operator()(const KeyStruct &K1, const KeyStruct &K2) const
    { int nd;
      for(nd=0; nd<KEYLEN; nd++)
       if ( fabs(K1.Key[nd] - K2.Key[nd]) > 1.0e-8 * fabs(K1.Key[nd]) )
        return false;
      return true;
    };

 } KeyCmp;

typedef std::tr1::unordered_map< KeyStruct,
                                 QIFIPPIData *, 
                                 KeyHash, 
                                 KeyCmp> KeyValueMap;

/*--------------------------------------------------------------*/
/*- class constructor ------------------------------------------*/
/*--------------------------------------------------------------*/
FIPPICache::FIPPICache()
{
  DoNotCompute=0;

  pthread_rwlock_init(&FCLock,0);

  KeyValueMap *KVM=new KeyValueMap;
  opTable = (void *)KVM;
}

/*--------------------------------------------------------------*/
/*- class destructor  ------------------------------------------*/
/*--------------------------------------------------------------*/
FIPPICache::~FIPPICache()
{
  KeyValueMap *KVM=(KeyValueMap *)opTable;
  delete KVM;
} 

/*--------------------------------------------------------------*/
/*- routine for fetching a FIPPI data record from a FIPPIDT:    */
/*- we look through our table to see if we have a record for    */
/*- this panel pair, we return it if we do, and otherwise we    */
/*- compute a new FIPPI data record for this panel pair and     */
/*- add it to the table.                                        */
/*- important note: the vertices are assumed to be canonically  */
/*- ordered on entry.                                           */
/*--------------------------------------------------------------*/
QIFIPPIData *FIPPICache::GetQIFIPPIData(double **OVa, double **OVb, int ncv)
{
  /***************************************************************/
  /* construct a search key from the canonically-ordered panel   */
  /* vertices.                                                   */
  /* the search key is kinda stupid, just a string of 15 doubles */
  /* as follows:                                                 */
  /* 0--2    VMed  - VMin [0..2]                                 */
  /* 3--5    VMax  - VMin [0..2]                                 */
  /* 6--8    VMinP - VMin [0..2]                                 */
  /* 9--11   VMedP - VMin [0..2]                                 */
  /* 12--14  VMaxP - VMin [0..2]                                 */
  /***************************************************************/
  KeyStruct K;
  VecSub(OVa[1], OVa[0], K.Key+0 );
  VecSub(OVa[2], OVa[0], K.Key+3 );
  VecSub(OVb[0], OVa[0], K.Key+6 );
  VecSub(OVb[1], OVa[0], K.Key+9 );
  VecSub(OVb[2], OVa[0], K.Key+12);

  /***************************************************************/
  /* look for this key in the cache ******************************/
  /***************************************************************/
  KeyValueMap *KVM=(KeyValueMap *)opTable;

  pthread_rwlock_rdlock(&FCLock);
  KeyValueMap::iterator p=KVM->find(K);
  pthread_rwlock_unlock(&FCLock);

  if ( p != (KVM->end()) )
   { Found++;
     return (QIFIPPIData *)(p->second);
   };
  
  /***************************************************************/
  /* if it was not found, allocate and compute a new QIFIPPIData */
  /* structure, then add this structure to the cache             */
  /***************************************************************/
  NotFound++;
  KeyStruct *K2 = (KeyStruct *)malloc(sizeof(*K2));
  memcpy(K2->Key, K.Key, KEYLEN*sizeof(double));
  QIFIPPIData *QIFD=(QIFIPPIData *)malloc(sizeof *QIFD);
  if (DoNotCompute==0)
   ComputeQIFIPPIData(OVa, OVb, ncv, QIFD);
   
  pthread_rwlock_wrlock(&FCLock);
  KVM->insert( KeyValuePair(*K2, QIFD) );
  pthread_rwlock_unlock(&FCLock);

  return QIFD;
}

/***************************************************************/
/* this routine and the following routine implement a mechanism*/
/* for storing the contents of a FIPPI cache to a binary file, */
/* and subsequently pre-loading a FIPPI cache with the content */
/* of a file created by this storage operation.                */
/*                                                             */
/* the file format is pretty simple (and non-portable w.r.t.   */
/* endianness):                                                */
/*  bytes 0--10:   'FIPPICACHE' + 0 (a file signature used as  */
/*                                   a simple sanity check)    */
/*  next xx bytes:  first record                               */
/*  next xx bytes:  second record                              */
/*  ...             ...                                        */
/*                                                             */
/* where xx is the size of the record; each record consists of */
/* a search key (15 double values) followed by the content     */
/* of the QIFIPPIDataRecord for that search key.               */
/*                                                             */
/* note: FIPPICF = 'FIPPI cache file'                          */
/***************************************************************/
const char FIPPICF_Signature[]="FIPPICACHE";
#define FIPPICF_SIGLEN sizeof(FIPPICF_Signature)

// note that this structure differs from the KeyValuePair structure 
// defined above in that it contains the actual contents of 
// QIFIPPIData structure, whereas KeyValuePair contains just a 
// pointer to such a structure.
typedef struct FIPPICF_Record
 { KeyStruct K;
   QIFIPPIData QIFDBuffer;
 } FIPPICF_Record;
#define FIPPICF_RECLEN sizeof(FIPPICF_Record)

void FIPPICache::Store(char *FileName)
{
  /*--------------------------------------------------------------*/
  /*- this whole routine is write-locked; presumably it will only */
  /*- ever be called from single-threaded code regions, but just  */
  /*- to be careful.                                              */
  /*--------------------------------------------------------------*/
  pthread_rwlock_wrlock(&FCLock);

  /*--------------------------------------------------------------*/
  /*- try to open the file ---------------------------------------*/
  /*--------------------------------------------------------------*/
  FILE *f=fopen(FileName,"w");
  if (!f)
   { fprintf(stderr,"warning: could not open file %s (aborting cache dump)...",FileName);
     pthread_rwlock_unlock(&FCLock);
     return;
   };
  Log("Writing FIPPI cache to file %s...",FileName);

  /*---------------------------------------------------------------------*/
  /*- write file signature ----------------------------------------------*/
  /*---------------------------------------------------------------------*/
  fwrite(FIPPICF_Signature,FIPPICF_SIGLEN,1,f);

  /*---------------------------------------------------------------------*/
  /*- iterate through the table and write entries to the file one-by-one */
  /*---------------------------------------------------------------------*/
  KeyValueMap *KVM=(KeyValueMap *)opTable;
  KeyValueMap::iterator it;
  KeyStruct K;
  QIFIPPIData *QIFD;
  FIPPICF_Record MyRecord;
  int NumRecords=0;
  for ( it = KVM->begin(); it != KVM->end(); it++ ) 
   { 
     K=it->first;
     QIFD=it->second;
     memcpy(&(MyRecord.K.Key),       K.Key, sizeof(MyRecord.K.Key ));
     memcpy(&(MyRecord.QIFDBuffer),  QIFD,  sizeof(MyRecord.QIFDBuffer));
     if ( 1 != fwrite(&(MyRecord),sizeof(MyRecord),1,f ) )
      break;
     NumRecords++;
   };

  /*---------------------------------------------------------------------*/
  /*- and that's it -----------------------------------------------------*/
  /*---------------------------------------------------------------------*/
  fclose(f);
  Log(" ...wrote %i FIPPI records.",NumRecords);
  pthread_rwlock_unlock(&FCLock);

}

void FIPPICache::PreLoad(char *FileName)
{

  pthread_rwlock_wrlock(&FCLock); 

  /*--------------------------------------------------------------*/
  /*- try to open the file ---------------------------------------*/
  /*--------------------------------------------------------------*/
  FILE *f=fopen(FileName,"r");
  if (!f)
   { fprintf(stderr,"warning: could not open file %s (skipping cache preload)\n",FileName);
     Log("Could not open FIPPI cache file %s...",FileName); 
     pthread_rwlock_unlock(&FCLock);
     return;
   };

  /*--------------------------------------------------------------*/
  /*- run through some sanity checks to make sure we have a valid */
  /*- cache file                                                  */
  /*--------------------------------------------------------------*/
  const char *ErrMsg=0;
  
  struct stat fileStats;
  if ( fstat(fileno(f), &fileStats) )
   ErrMsg="invalid cache file";
  
  // check that the file signature is present and correct 
  off_t FileSize=fileStats.st_size;
  char FileSignature[FIPPICF_SIGLEN]; 
  if ( ErrMsg==0 && FileSize < FIPPICF_SIGLEN )
   ErrMsg="invalid cache file";
  if ( ErrMsg==0 && 1!=fread(FileSignature, FIPPICF_SIGLEN, 1, f) )
   ErrMsg="invalid cache file";
  if ( ErrMsg==0 && strcmp(FileSignature, FIPPICF_Signature) ) 
   ErrMsg="invalid cache file";

  // the file size, minus the portion taken up by the signature, 
  // should be an integer multiple of the size of a FIPPICF_Record
  FileSize-=FIPPICF_SIGLEN;
  if ( ErrMsg==0 && (FileSize % FIPPICF_RECLEN)!=0 )
   ErrMsg="cache file has incorrect size";


  /*--------------------------------------------------------------*/
  /*- allocate memory to store cache entries read from the file. -*/
  /*- for now, we abort if there is not enough memory to store   -*/
  /*- the full cache; TODO explore schemes for partial preload.  -*/
  /*--------------------------------------------------------------*/
  unsigned int NumRecords = FileSize / FIPPICF_RECLEN;
  FIPPICF_Record *Records;
  if ( ErrMsg==0 )
   { Records=(FIPPICF_Record *)malloc(NumRecords*FIPPICF_RECLEN);
     if ( !Records)
      ErrMsg="insufficient memory to preload cache";
   };

  /*--------------------------------------------------------------*/
  /*- pause here to clean up if anything went wrong --------------*/
  /*--------------------------------------------------------------*/
  if (ErrMsg)
   { fprintf(stderr,"warning: file %s: %s (skipping cache preload)\n",FileName,ErrMsg);
     Log("FIPPI cache file %s: %s (skipping cache preload)",FileName,ErrMsg);
     fclose(f);
     pthread_rwlock_unlock(&FCLock);
     return;
   };

  /*--------------------------------------------------------------*/
  /*- now just read records from the file one at a time and add   */
  /*- them to the table.                                          */
  /*--------------------------------------------------------------*/
  KeyValueMap *KVM=(KeyValueMap *)opTable;
  int nr;
  Log("Preloading FIPPI records from file %s...",FileName);
  for(nr=0; nr<NumRecords; nr++)
   { 
     if ( fread(Records+nr, FIPPICF_RECLEN,1,f) != 1 )
      { fprintf(stderr,"warning: file %s: read only %i of %i records",FileName,nr+1,NumRecords);
        fclose(f);
        pthread_rwlock_unlock(&FCLock);
        return;
      };

     KVM->insert( KeyValuePair(Records[nr].K, &(Records[nr].QIFDBuffer)) );
   };

  /*--------------------------------------------------------------*/
  /*- the full file was successfully preloaded -------------------*/
  /*--------------------------------------------------------------*/
  Log(" ...succesfully preloaded %i FIPPI records.",NumRecords);
  fclose(f);
  pthread_rwlock_unlock(&FCLock);

}

/***************************************************************/
/***************************************************************/
/***************************************************************/
FIPPICache GlobalFIPPICache;

void PreloadGlobalFIPPICache(char *FileName)
{ 
  GlobalFIPPICache.PreLoad(FileName);
}

void StoreGlobalFIPPICache(char *FileName)
{ 
  GlobalFIPPICache.Store(FileName);
}

} // namespace scuff
