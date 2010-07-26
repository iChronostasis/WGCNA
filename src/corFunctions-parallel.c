

/* 
  Calculation of adjacency and Topological overlap. More efficient and hopefully faster than R code.


  Compiling:

  gcc --std=c99 -fPIC -O3 -lpthread -lm -o functions.so -shared \
     -I/usr/local/lib/R-2.7.1-goto/include \
     functions-parallel.c pivot.c

  Home:

  gcc --std=c99 -fPIC -O3 -lpthread -lm -o functions.so -shared \
     -I/usr/local/lib/R-2.8.0-patched-2008-12-06-Goto/include \
     functions-parallel.c pivot.c

  Titan: not working yet. need the threads library for Windows.

  gcc --std=c99 -fPIC -O3 -lpthread -lm -o functions.dll -shared \
     -IM:/R/R-27~0PA/include \
     functions-parallel.c pivot.c




 
 gcc --std=gnu99 --shared -Ic:/PROGRA~1/R/R-27~0PA/include -o functions.dll functions.c -Lc:/PROGRA~1/R/R-27~0PA/bin -lR -lRblas

"C:\Program Files\R\R-2.7.0pat\bin\R.exe" CMD SHLIB functions.c -lRblas

Copyright (C) 2008 Peter Langfelder; parts based on R by R Development team

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.



Some notes on handling of zero MAD:
(.) in the threaded calculations, each columns has its own NAmed, but the zeroMAD flag is one flag per thread.
    Thus, it should be zeroed out before the threaded calculation starts and checked at the end.

*/

int uselessFunction2()
{
  int a = 2;
  return a;
}

#ifdef WITH_THREADS

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>

#include <sys/time.h>

#include <R.h>
#include <R_ext/BLAS.h>
#include <R_ext/libextern.h>
#define LDOUBLE 	long double

#include "pivot.h"

#include "corFunctions-common.h"

/*========================================================================
 *
 * Short test code to see whether parallel code can be incorporated into R
 *
 * =======================================================================
 */

int nProcessors()
{
#ifdef _SC_NPROCESSORS_CONF
  long nProcessorsOnline = sysconf(_SC_NPROCESSORS_ONLN);
#else
  long nProcessorsOnline = 2;
#endif
  return (int) nProcessorsOnline;
}

#define MxThreads      128

#define AllowedPropNAChange	0.01

typedef struct 
{
   volatile int i, n;
}  progressCounter;

/* For each parallel operation will presumably need a separate structure to hold its
 * information, but can define a common structure holding the general information that is needed to
 * calculate correlation. Can keep two versions, one for calculating cor(x), one for cor(x,y).
 * Each specific thread-task specific struct can contain a pointer to the general structure.
 */

// General information for a [bi]cor(x) calculation

typedef struct
{
   double * x;
   int nr, nc;
   double * multMat, * result;
   double * aux;
   int *nNAentries, *NAme;
   int zeroMAD;
   double maxPOutliers;
   double quick;
   int robust, fallback;
   int id;
}  cor1ThreadData;

// General information for a [bi]cor(x,y) calculation

typedef struct
{
   cor1ThreadData * x, * y;
}  cor2ThreadData;

// Information for column preparation

typedef struct
{
   cor1ThreadData * x;
   progressCounter * pc;
   pthread_mutex_t * lock;
}  colPrepThreadData;

// Information for symmetrization

typedef struct
{
   cor1ThreadData * x;
   progressCounter * pc;
}  symmThreadData;

// Information for threaded slow calculations for cor1

typedef struct
{
   cor1ThreadData * x;
   progressCounter * pci, * pcj;
   int * nSlow, * nNA;
   pthread_mutex_t * lock;
}  slowCalcThreadData;


/*======================================================================================
 *
 * prepareColBicor
 *
 * =====================================================================================*/

void * threadPrepColBicor(void * par)
{
  colPrepThreadData * td = (colPrepThreadData *) par;
  cor1ThreadData * x = td->x;

  // Rprintf("Preparing columns: nr = %d, nc = %d\n", x->nr, x->nc);
  while (td->pc->i < td->pc->n)
  {
      // Grab the next column that needs to be done
      pthread_mutex_lock( td->lock );
      if (td->pc->i < td->pc->n)
      {
         int col = td->pc->i;
         // Rprintf("...working on column %d\n", col);
         td->pc->i++;
         pthread_mutex_unlock( td->lock );
 
         prepareColBicor(x->x + col * x->nr, 
                         x->nr, 
                         x->maxPOutliers, 
                         x->fallback, 
                         x->multMat + col * x->nr,
                         x->nNAentries + col,
                         x->NAme + col,
                         &(x->zeroMAD),
                         x->aux,
                         x->aux + x->nr);
         // if (x->zeroMAD > 0) { Rprintf("..mad was zero in column %d.\n", td->pc->i); }
         if ( (x->zeroMAD > 0) && (x->fallback==3)) { x->zeroMAD = td->pc->i; td->pc->i = td->pc->n; }
      } else 
         pthread_mutex_unlock( td->lock );
  }
  return NULL;
} 
      

/*======================================================================================
 *
 * prepareColCor
 *
 * =====================================================================================*/

// Used for the fast calculation of Pearson correlation
// and when bicor is called with robustsX or robustY = 0


void * threadPrepColCor(void * par)
{
  colPrepThreadData * td = (colPrepThreadData *) par;
  cor1ThreadData * x = td->x;
  while (td->pc->i < td->pc->n)
  {
      // Grab the next column that needs to be done
      pthread_mutex_lock( td->lock );
      int col = td->pc->i;
      if (col < td->x->nc)
      {
         td->pc->i++;
         pthread_mutex_unlock( td->lock );
 
         prepareColCor(x->x + col * x->nr, 
                       x->nr, 
                       x->multMat + col * x->nr,
                       x->nNAentries + col,
                       x->NAme + col);
      } else 
         pthread_mutex_unlock( td->lock );
  }
  return NULL;
} 
      
/*===================================================================================================
 *
 * Threaded symmetrization and NA'ing out of rows and columns with NA means
 *
 *===================================================================================================
*/

void * threadSymmetrize(void * par)
{
  symmThreadData * td = (symmThreadData *) par;
  cor1ThreadData * x = td->x;

  int nc = x->nc;
  double * result = x->result;
  int * NAmean = x->NAme;
  int col = 0;
  while ( (col = td->pc->i) < nc)
  {
      // Symmetrize the column
      // point counter to the next column
      td->pc->i = col+1;
      // and update the matrix. Start at j=col to check for values greater than 1.
      if (NAmean[col] == 0)
      {
        double * resx = result + col*nc + col;
        // Rprintf("Symmetrizing column %d to the same row.\n", col);
        for (int j=col; j<nc; j++) 
        {
          if (NAmean[j] == 0)
          {
            if (!ISNAN(*resx))
            {
               if (*resx > 1.0) *resx = 1.0;
               if (*resx < -1.0) *resx = -1.0;
            }
            result[j*nc + col] = *resx;
          }
          resx ++;
        }
      } else {
        // Rprintf("NA-ing out column and row %d\n", col);
        for (int j=0; j<nc; j++)
        {
           result[col*nc + j] = NA_REAL;
           result[j*nc + col] = NA_REAL;
        }
      }
  }
  return NULL;
} 
      
/*===================================================================================================
 *
 * Threaded "slow" calculations for bicor
 *
 *===================================================================================================
*/
// This can actually be relatively slow, since the search for calculations that need to be done is not
// parallel, so one thread may have to traverse the whole matrix. I can imagine parallelizing even that
// part, but for now leave it as is as this will at best be a minuscule improvement.

void * threadSlowCalcBicor(void * par)
{
  slowCalcThreadData * td = (slowCalcThreadData *) par;
  int * nSlow = td->nSlow;
  int * nNA = td->nNA;
  double * x = td->x->x;
  double * multMat = td->x->multMat;
  double * result = td->x->result;
  int fbx = td->x->fallback;
  int nc = td->x->nc, nc1 = nc-1, nr = td->x->nr;
  int * NAmean = td->x->NAme;
  int * nNAentries = td->x->nNAentries;
  progressCounter * pci = td->pci, * pcj = td->pcj;

  double maxPOutliers = td->x->maxPOutliers;

  double * xx = td->x->aux, * yy = xx + nr; 
  double * xxx = xx + 2*nr, * yyy = xx + 3*nr;
  double * xx2 = xx + 4*nr, * yy2 = xx + 5*nr;

  int maxDiffNA = (int) (td->x->quick * nr);

  if (fbx==3) fbx = 2; // For these calculations can't go back and redo everything

 
  // Rprintf("Checking %d rows and %d columns\n", nc1, nc);
  // Rprintf("starting at %d and %d\n", pci->i, pcj->i);
  while (pci->i < nc1)
  {
     pthread_mutex_lock( td->lock );
     int i = pci->i, ii = i;
     int j = pcj->i, jj = j;
     do
     {
       i = ii;
       j = jj;
       jj++;
       if (jj==nc) 
       {
         ii++;
         jj = ii+1;
       }
     } while ((i<nc1) && (j<nc) && 
              ((NAmean[i] > 0) || (NAmean[j] > 0) ||
               ( (nNAentries[i] <= maxDiffNA) && ( nNAentries[j] <= maxDiffNA))));
     pci->i = ii;
     pcj->i = jj;
     pthread_mutex_unlock( td->lock );
 
     if ((i < nc1) && (j < nc) )
     {
        // Rprintf("Recalculating row %d and column %d, column size %d\n", i, j, nr);
        memcpy((void *)xx, (void *)(x + i*nr), nr * sizeof(double));
        memcpy((void *)yy, (void *)(x + j*nr), nr * sizeof(double));

        int nNAx = 0, nNAy = 0;    
        for (int k=0; k<nr; k++)
        {
           if (ISNAN(xx[k])) yy[k] = NA_REAL;
           if (ISNAN(yy[k])) xx[k] = NA_REAL;
           if (ISNAN(xx[k])) nNAx++;
           if (ISNAN(yy[k])) nNAy++;
        }
        int NAx = 0, NAy = 0;

        if ((nNAx - nNAentries[i] > maxDiffNA) || (nNAy-nNAentries[j] > maxDiffNA))
        {
            // must recalculate the auxiliary variables for both columns
            int temp = 0, zeroMAD = 0;
            if (nNAx - nNAentries[i] > maxDiffNA)
                  prepareColBicor(xx, nr, maxPOutliers, fbx, xxx, &temp, &NAx, &zeroMAD, xx2, yy2);
               else
                  memcpy((void *) xxx, (void *) (multMat + i * nr),  nr * sizeof(double));
            if (nNAy-nNAentries[j] > maxDiffNA)
                  prepareColBicor(yy, nr, maxPOutliers, fbx, yyy, &temp, &NAy, &zeroMAD, xx2, yy2);
               else
                  memcpy((void *) yyy, (void *) (multMat + j * nr),  nr * sizeof(double));
            if (NAx + NAy==0)
            {
               LDOUBLE sumxy = 0;
               int count = 0;
               for (int k=0; k<nr; k++)
               {
                 double vx = *(xxx + k), vy = *(yyy +  k);
                 // Rprintf("i: %d, j: %d, k: %d: vx: %e, vy: %e\n", i,j,k,vx,vy);
                 if (!ISNAN(vx) && !ISNAN(vy))
                 {
                   sumxy += vx * vy; 
                   count++;
                 }
               }
               if (count==0) 
               {
                  result[i*nc + j] = NA_REAL; 
                  (*nNA)++;
               } else {
                  result[i*nc + j] = (double) sumxy;
               }
             } else {
                result[i*nc + j] = NA_REAL; 
                (*nNA)++;
             }
             // result[j*nc + i] = result[i*nc + j];
             (*nSlow)++;
        }
     }
  }
  return NULL;
}

/*===================================================================================================
 *
 * Threaded "slow" calculations for pearson correlation
 *
 *===================================================================================================
*/
// This can actually be relatively slow, since the search for calculations that need to be done is not
// parallel, so one thread may have to traverse the whole matrix. I can imagine parallelizing even that
// part, but for now leave it as is as this will at best be a minuscule improvement.

void * threadSlowCalcCor(void * par)
{
  slowCalcThreadData * td = (slowCalcThreadData *) par;
  int * nSlow = td->nSlow;
  int * nNA = td->nNA;
  double * x = td->x->x;
  double * result = td->x->result;
  int nc = td->x->nc, nc1 = nc-1, nr = td->x->nr;
  int * NAmean = td->x->NAme;
  int * nNAentries = td->x->nNAentries;
  progressCounter * pci = td->pci, * pcj = td->pcj;

  double *xx, *yy;
  double vx, vy;

  int maxDiffNA = (int) (td->x->quick * nr);

  // Rprintf("quick:%f\n", td->x->quick);


  // Rprintf("Checking %d rows and %d columns\n", nc1, nc);
  // Rprintf("starting at %d and %d\n", pci->i, pcj->i);
  while (pci->i < nc1)
  {
     pthread_mutex_lock( td->lock );
     int i = pci->i, ii = i;
     int j = pcj->i, jj = j;
     do
     {
       i = ii;
       j = jj;
       jj++;
       if (jj==nc) 
       {
         ii++;
         jj = ii+1;
       }
     } while ((i<nc1) && (j<nc) && 
               ((NAmean[i] > 0) || (NAmean[j] > 0) ||
                ( (nNAentries[i] <= maxDiffNA) && ( nNAentries[j] <= maxDiffNA))));

     pci->i = ii;
     pcj->i = jj;
     pthread_mutex_unlock( td->lock );
 
     if ((i < nc1) && (j < nc))
     {
        // Rprintf("Recalculating row %d and column %d, column size %d\n", i, j, nr);
        xx = x + i * nr; yy = x + j * nr;
        LDOUBLE sumxy = 0, sumx = 0, sumy = 0, sumxs = 0, sumys = 0;
        int count = 0;
       
        for (int k=0; k<nr; k++)
        {
           vx = *xx; vy = *yy;
           if (!ISNAN(vx) && !ISNAN(vy))
           {
             count ++;
             sumxy += vx * vy;
             sumx += vx;
             sumy += vy;
             sumxs += vx*vx;
             sumys += vy*vy;
           }
           xx++; yy++;
        }
        if (count==0) 
        {
            result[i*nc + j] = NA_REAL; 
            (*nNA)++;
        } else {
            result[i*nc + j] = (double) ( (sumxy - sumx * sumy/count)/
                                sqrtl( (sumxs - sumx*sumx/count) * (sumys - sumy*sumy/count) ) );
        }
        // result[j*nc + i] = result[i*nc + j];
        //Rprintf("Incrementing nSlow: i=%d, j=%d, nNAentries[i]=%d, nNAentries[j]=%d, maxDiffNA=%d\n", 
          //      i, j, nNAentries[i], nNAentries[j], maxDiffNA);
        (*nSlow)++;
     }
  }
  return NULL;
}

//===================================================================================================

// Pearson correlation of a matrix with itself.
// This one uses matrix multiplication in BLAS to speed up calculation when there are no NA's
// and uses threading to speed up the rest of the calculation.

//===================================================================================================


void cor1Fast(double * x, int * nrow, int * ncol, double * quick, double * result, int *nNA, int * err, 
          int * nThreads,
          int * verbose, int * indent)
{
  int nr = *nrow, nc = *ncol;

  char          spaces[2* *indent+1];

  for (int i=0; i<2* *indent; i++) spaces[i] = ' ';
  spaces[2* *indent] = '\0';

  *nNA = 0;

  // Allocate space for various variables

  double * multMat;
  int * nNAentries, *NAmean;

  // This matrix will hold preprocessed entries that can be simply multiplied together to get the
  // numerator

  if ( (multMat = malloc(nc*nr * sizeof(double)))==NULL )
  {
    *err = 1;
    Rprintf("cor1: memmory allocation error. If possible, please decrease block size.\n");
    return;
  }

  // Number of NA entries in each column

  if ( (nNAentries = malloc(nc * sizeof(int)))==NULL )
  {
    free(multMat);
    *err = 1;
    Rprintf("cor1: memmory allocation error. The needed block is relatively small... suspicious.\n");
    return;
  }

  // Flag indicating whether the mean of each column is NA

  if ( (NAmean = malloc(nc * sizeof(int)))==NULL )
  {
    free(nNAentries); free(multMat); 
    *err = 1;
    Rprintf("cor1: memmory allocation error. The needed block is relatively small... suspicious.\n");
    return;
  }

  // Calculate multMat = (x - mean(x))/sqrt(sum(x - mean(x)))
  // Use a threaded calculation for this...
  
  int nt = *nThreads;
  if ((nt < 1) || (nt > MxThreads))
  {
    nt = nProcessors();
    if (nt >MxThreads) nt = MxThreads;
  }
  if (*verbose)
      Rprintf("%s..will use %d parallel threads.\n", spaces, nt);

  // double * aux[MxThreads];

  // for (int t=0; t < nt; t++)
  // {
     // if ( (aux[t] = malloc(6*nr * sizeof(double)))==NULL)
     // {
       // *err = 1;
       // Rprintf("cor1: memmory allocation error. The needed block is very small... suspicious.\n");
       // for (int tt = t-1; tt>=0; tt--) free(aux[tt]);
       // free(NAmean); free(nNAentries); free(multMat);
       // return;
     // }
  // }

  // Put the general data of the correlation calculation into a structure that can be passed on to
  // threads.

  cor1ThreadData thrdInfo[MxThreads];
  for (int t = 0; t < nt; t++)
  {
     thrdInfo[t].x = x;
     thrdInfo[t].nr = nr;
     thrdInfo[t].nc = nc;
     thrdInfo[t].multMat = multMat;
     thrdInfo[t].result = result;
     thrdInfo[t].nNAentries = nNAentries;
     thrdInfo[t].NAme = NAmean;
     thrdInfo[t].quick = *quick;
     thrdInfo[t].id = t;
  }

  // Column preparation (calculation of the matrix to be multiplied) in a threaded form.

  colPrepThreadData  cptd[MxThreads];
  pthread_t  thr[MxThreads];
  int       status[MxThreads];

  pthread_mutex_t mutex1 = PTHREAD_MUTEX_INITIALIZER;
  progressCounter pc;

  pc.i = 0;
  pc.n = nc;

  // Rprintf("Preparing columns...\n");
  for (int t=0; t<nt; t++)
  {
    cptd[t].x = &thrdInfo[t];
    cptd[t].pc = &pc;
    cptd[t].lock = &mutex1;
    status[t] = pthread_create(&thr[t], NULL, threadPrepColCor, (void *) &cptd[t]);
  }

  for (int t=0; t<nt; t++)
      pthread_join(thr[t], NULL);

  // Rprintf("done...\n");
  // Rprintf("NAmean:");
  // for (int i=0; i<nc; i++) Rprintf(" %d,", NAmean[i]);
  // Rprintf("\n");

  // The main loop is actually a matrix multiplication

  double alpha = 1.0, beta = 0.0;
  dsyrk_("L", "T", ncol, nrow, & alpha, multMat, nrow, & beta, result, ncol);

  int nSlow = 0;

  // Rprintf("nNAentries values: ");
  // for (int i = 0; i < nc; i++) Rprintf("%d, ", nNAentries[i]);
  // Rprintf("\n");
  //
  //
  
  if (*quick < 1.0)
  {
      // Parallelized slow calculations
      slowCalcThreadData  sctd[MxThreads];
      progressCounter pci, pcj;
      pthread_mutex_t mutexSC = PTHREAD_MUTEX_INITIALIZER;

      pthread_t  thr3[MxThreads];

      pci.i = 0;
      pci.n = nc;
      pcj.i = 1;
      pcj.n = nc;

      // Rprintf("slow calculations... nt=%d\n", nt);
      for (int t=0; t<nt; t++)
      {
        sctd[t].x = &thrdInfo[t];
        sctd[t].pci = &pci;
        sctd[t].pcj = &pcj;
        sctd[t].nSlow = &nSlow;
        sctd[t].nNA = nNA;
        sctd[t].lock = &mutexSC;
        status[t] = pthread_create(&thr3[t], NULL, threadSlowCalcCor, (void *) &sctd[t]);
      }
    
      for (int t=0; t<nt; t++)
          pthread_join(thr3[t], NULL);
    
      // Rprintf("done...\n");
    
      if (*verbose) Rprintf("%s Fraction of slow calculations: %f\n", spaces, 
                             ( (double) nSlow*2 ) / (nc*(nc-1)) );
  }
      
  // Symmetrize the result and set all rows and columns with NA means to zero
  
  symmThreadData  std[MxThreads];
  // reset the progress counter
  pc.i = 0;
  pc.n = nc;

  pthread_t  thr2[MxThreads];

  // Rprintf("symmetrizing... nt=%d\n", nt);
  for (int t=0; t<nt; t++)
  {
    std[t].x = &thrdInfo[t];
    std[t].pc = &pc;
    status[t] = pthread_create(&thr2[t], NULL, threadSymmetrize, (void *) &std[t]);
  }

  for (int t=0; t<nt; t++)
      pthread_join(thr2[t], NULL);

  // Rprintf("done... nt=%d\n", nt);
  // Here I need to recalculate results that have NA's in them.

  // for (int t=nt-1; t >= 0; t--) free(aux[t]);
  free(NAmean);
  free(nNAentries);
  free(multMat);
}


//===================================================================================================

// bicorrelation of a matrix with itself.
// This one uses matrix multiplication in BLAS to speed up calculation when there are no NA's
// and is threaded to speed up the rest of the calculation.

//===================================================================================================


void bicor1Fast(double * x, int * nrow, int * ncol, double * maxPOutliers, 
            double * quick, int * fallback,
            double * result, int *nNA, int * err, 
            int * nThreads,
            int * verbose, int * indent)
{
  int nr = *nrow, nc = *ncol;

  char          spaces[2* *indent+1];

  for (int i=0; i<2* *indent; i++) spaces[i] = ' ';
  spaces[2* *indent] = '\0';

  *nNA = 0;

  // Allocate space for various variables

  double * multMat;
  int * nNAentries, *NAmed;

  if ( (multMat = malloc(nc*nr * sizeof(double)))==NULL )
  {
    *err = 1;
    Rprintf("cor1: memmory allocation error. If possible, please decrease block size.\n");
    return;
  }

  // Number of NA entries in each column

  if ( (nNAentries = malloc(nc * sizeof(int)))==NULL )
  {
    free(multMat);
    *err = 1;
    Rprintf("cor1: memmory allocation error. The needed block is relatively small... suspicious.\n");
    return;
  }

  // Flag indicating whether the mean of each column is NA

  if ( (NAmed = malloc(nc * sizeof(int)))==NULL )
  {
    free(nNAentries); free(multMat); 
    *err = 1;
    Rprintf("cor1: memmory allocation error. The needed block is relatively small... suspicious.\n");
    return;
  }

  // Calculate multMat = (x - mean(x))/sqrt(sum(x - mean(x)))
  // Use a threaded calculation for this...
  
  int nt = *nThreads;
  if ((nt < 1) || (nt > MxThreads))
  {
    nt = nProcessors();
    if (nt >MxThreads) nt = MxThreads;
  }

  if (*verbose)
      Rprintf("%s..will use %d parallel threads.\n", spaces, nt);

  double * aux[MxThreads];

  double * aux1[MxThreads];

  for (int t=0; t < nt; t++)
  {
     if ( (aux[t] = malloc(6*nr * sizeof(double)))==NULL)
     {
       *err = 1;
       Rprintf("cor1: memmory allocation error. The needed block is very small... suspicious.\n");
       for (int tt = t-1; tt>=0; tt--) free(aux[tt]);
       free(NAmed); free(nNAentries); free(multMat);
       return;
     }
     aux1[t] = aux[t];
  }

  // Put the general data of the correlation calculation into a structure that can be passed on to
  // threads.

  cor1ThreadData thrdInfo[MxThreads];
  for (int t = 0; t < nt; t++)
  {
     thrdInfo[t].x = x;
     thrdInfo[t].nr = nr;
     thrdInfo[t].nc = nc;
     thrdInfo[t].multMat = multMat;
     thrdInfo[t].result = result;
     thrdInfo[t].nNAentries = nNAentries;
     thrdInfo[t].NAme = NAmed;
     thrdInfo[t].zeroMAD = 0; 
     thrdInfo[t].aux = aux[t];
     thrdInfo[t].robust = 0;
     thrdInfo[t].fallback = *fallback;
     thrdInfo[t].quick = *quick;
     thrdInfo[t].maxPOutliers = *maxPOutliers;
     thrdInfo[t].id = t;
  }

  // Column preparation (calculation of the matrix to be multiplied) in a threaded form.

  colPrepThreadData  cptd[MxThreads];
  pthread_t  thr[MxThreads];
  int       status[MxThreads];

  pthread_mutex_t mutex1 = PTHREAD_MUTEX_INITIALIZER;
  progressCounter pc;

  pc.i = 0;
  pc.n = nc;

  // Rprintf("Preparing columns...\n");
  for (int t=0; t<nt; t++)
  {
    cptd[t].x = &thrdInfo[t];
    cptd[t].pc = &pc;
    cptd[t].lock = &mutex1;
    status[t] = pthread_create(&thr[t], NULL, threadPrepColBicor, (void *) &cptd[t]);
  }

  for (int t=0; t<nt; t++)
      pthread_join(thr[t], NULL);

  int pearson = 0;

  if (*fallback==3)
  {
    for (int t=0; t<nt; t++) if (thrdInfo[t].zeroMAD > 0)
    { 
      pearson = 1;
      if (*verbose)
        Rprintf("Warning in bicor(x): MAD is zero in column %d. Switching to Pearson correlation.\n", 
                 thrdInfo[t].zeroMAD);
    }
    if (pearson==1) // Re-do all column preparations using Pearson preparation.
    { 
      // Set fallback to 4 for slow calculations below.
      for (int t = 0; t < nt; t++) thrdInfo[t].fallback = 4;

      pthread_mutex_t mutex2 = PTHREAD_MUTEX_INITIALIZER;
      pc.i = 0;
      pc.n = nc;

      for (int t=0; t<nt; t++)
      {
        cptd[t].lock = &mutex2;
        status[t] = pthread_create(&thr[t], NULL, threadPrepColCor, (void *) &cptd[t]);
      }
      for (int t=0; t<nt; t++) pthread_join(thr[t], NULL);
    }
  }

  // Rprintf("done...\n");
  // Rprintf("NAmed:");
  // for (int i=0; i<nc; i++) Rprintf(" %d,", NAmed[i]);
  // Rprintf("\n");

  // The main loop is actually a matrix multiplication

  double alpha = 1.0, beta = 0.0;
  dsyrk_("L", "T", ncol, nrow, & alpha, multMat, nrow, & beta, result, ncol);

  // Here I need to recalculate results that have NA's in them.

  int nSlow = 0;

  // Rprintf("nNAentries values: ");
  // for (int i = 0; i < nc; i++) Rprintf("%d, ", nNAentries[i]);
  // Rprintf("\n");
  //
  //
  
  if (*quick < 1)
  {
      // Parallelized slow calculations
      slowCalcThreadData  sctd[MxThreads];
      progressCounter pci, pcj;
      pthread_mutex_t mutexSC = PTHREAD_MUTEX_INITIALIZER;

      pthread_t  thr3[MxThreads];

      pci.i = 0;
      pci.n = nc;
      pcj.i = 1;
      pcj.n = nc;

      // Rprintf("slow calculations... nt=%d\n", nt);
      for (int t=0; t<nt; t++)
      {
        sctd[t].x = &thrdInfo[t];
        sctd[t].pci = &pci;
        sctd[t].pcj = &pcj;
        sctd[t].nSlow = &nSlow;
        sctd[t].nNA = nNA;
        sctd[t].lock = &mutexSC;
        status[t] = pthread_create(&thr3[t], NULL, threadSlowCalcBicor, (void *) &sctd[t]);
      }
    
      for (int t=0; t<nt; t++)
          pthread_join(thr3[t], NULL);
    
      // Rprintf("done...\n");
    
      if (*verbose) Rprintf("%s Fraction of slow calculations: %f\n", spaces, 
                             ( (double) nSlow*2 ) / (nc*(nc-1)) );
  }
      
  // Symmetrize the result and set all rows and columns with NA means to zero
  
  symmThreadData  std[MxThreads];
  // reset the progress counter
  pc.i = 0;
  pc.n = nc;

  pthread_t  thr2[MxThreads];

  // Rprintf("symmetrizing... nt=%d\n", nt);
  for (int t=0; t<nt; t++)
  {
    std[t].x = &thrdInfo[t];
    std[t].pc = &pc;
    status[t] = pthread_create(&thr2[t], NULL, threadSymmetrize, (void *) &std[t]);
  }

  for (int t=0; t<nt; t++)
      pthread_join(thr2[t], NULL);

  for (int t=nt-1; t >= 0; t--) free(aux[t]); 
  free(NAmed);
  free(nNAentries);
  free(multMat);
}

/*==============================================================================================
 *
 * Threaded 2-variable versions of the correlation functions
 *
 *==============================================================================================
*/


typedef struct
{
   cor2ThreadData * x;
   progressCounter * pci, *pcj;
   int * nSlow, * nNA;
   pthread_mutex_t * lock;
   double quick;
}  slowCalc2ThreadData;


// Data for NAing out appropriate rows and columns

typedef struct
{
   cor2ThreadData * x; 
   progressCounter * pci, *pcj;
}  NA2ThreadData;

/*===================================================================================================
 *
 * Threaded NA'ing out of rows and columns with NA means
 * and checking that all values are smaller than 1.
 *
 *===================================================================================================
*/

void * threadNAing(void * par)
{
  NA2ThreadData * td = (NA2ThreadData *) par;

  double * result = td->x->x->result;
  int ncx = td->x->x->nc;
  int * NAmedX = td->x->x->NAme;

  int ncy = td->x->y->nc;
  int * NAmedY = td->x->y->NAme;

  progressCounter * pci = td->pci;
  progressCounter * pcj = td->pcj;

  // Go row by row

  int row = 0, col = 0;

  while  ((row = pci->i) < ncx)
  {
      pci->i = row + 1;
      if (NAmedX[row])
      {
         // Rprintf("NA-ing out column and row %d\n", col);
         for (int j=0; j<ncy; j++)
              result[row + j * ncx] = NA_REAL;
      } 
  }

  // ... and column by column

  while ( (col = pcj->i) < ncy)
  {
         pcj->i = col + 1;
         if (NAmedY[col])
         {
            // Rprintf("NA-ing out column and row %d\n", col);
            for (int i=0; i<ncx; i++)
                 result[i + col * ncx] = NA_REAL;
         } else {
            double *resx = result + col*ncx;
            for (int i=0; i<ncx; i++) 
            {
               if (!ISNAN(*resx))
               {
                 if (*resx > 1.0) *resx = 1.0;
                 if (*resx < -1.0) *resx = -1.0;
               }
               resx++;
            }
         }
  }

  return NULL;
} 


/*===================================================================================================
 *
 * Threaded "slow" calculations for bicor(x,y)
 *
 *===================================================================================================
*/
// This can actually be relatively slow, since the search for calculations that need to be done is not
// parallel, so one thread may have to traverse the whole matrix. I can imagine parallelizing even that
// part, but for now leave it as is as this will at best be a minuscule improvement.

void * threadSlowCalcBicor2(void * par)
{
  slowCalc2ThreadData * td = (slowCalc2ThreadData *) par;
  int * nSlow = td->nSlow;
  int * nNA = td->nNA;

  double * x = td->x->x->x;
  double * multMatX = td->x->x->multMat;
  double * result = td->x->x->result;
  int ncx = td->x->x->nc, nr = td->x->x->nr;
  int * NAmeanX = td->x->x->NAme;
  int * nNAentriesX = td->x->x->nNAentries;
  int robustX = td->x->x->robust;
  int fbx = td->x->x->fallback;

  double * y = td->x->y->x;
  double * multMatY = td->x->y->multMat;
  int ncy = td->x->y->nc;
  int * NAmeanY = td->x->y->NAme;
  int * nNAentriesY = td->x->y->nNAentries;
  int robustY = td->x->y->robust;
  int fby = td->x->y->fallback;

  double maxPOutliers = td->x->x->maxPOutliers;

  progressCounter * pci = td->pci, * pcj = td->pcj;

  double * xx = td->x->x->aux;
  double * xxx = xx + nr;
  double * xx2 = xx + 2*nr;

  double * yy = td->x->y->aux;
  double * yyy = yy + nr;
  double * yy2 = yy + 2*nr;

  double * xx3, *yy3;

  int maxDiffNA = (int) (td->x->x->quick * nr);

  int temp = 0;

  if (fbx==3) fbx = 2;
  if (fby==3) fby = 2;

  if (!robustX) fbx = 4;
  if (!robustY) fby = 4;

  // Rprintf("Remedial calculation thread #%d: starting at %d and %d\n", td->x->x->id, 
  //            pci->i, pcj->i);
  //

  while (pci->i < ncx)
  {
     pthread_mutex_lock( td->lock );
     int i = pci->i, ii = i;
     int j = pcj->i, jj = j;
     do
     {
       i = ii;
       j = jj;
       jj++;
       if (jj==ncy) 
       {
         ii++;
         jj = 0;
       }
     } while ((i<ncx) && (j<ncy) && 
              ((NAmeanX[i] > 0) || (NAmeanY[j] > 0) || 
                ( (nNAentriesX[i] <= maxDiffNA) && ( nNAentriesY[j] <= maxDiffNA))));
     pci->i = ii;
     pcj->i = jj;
     pthread_mutex_unlock( td->lock );
 
     if ((i < ncx) && (j < ncy))
     {
        memcpy((void *)xx, (void *)(x + i*nr), nr * sizeof(double));
        memcpy((void *)yy, (void *)(y + j*nr), nr * sizeof(double));

        int nNAx = 0, nNAy = 0;    
        for (int k=0; k<nr; k++)
        {
           if (ISNAN(xx[k])) yy[k] = NA_REAL;
           if (ISNAN(yy[k])) xx[k] = NA_REAL;
           if (ISNAN(xx[k])) nNAx++;
           if (ISNAN(yy[k])) nNAy++;
        }
        int NAx = 0, NAy = 0;

        if ((nNAx - nNAentriesX[i] > maxDiffNA) || (nNAy-nNAentriesY[j] > maxDiffNA))
        {
            // Rprintf("Recalculating row %d and column %d, column size %d in thread %d\n", i, j, nr,
            //         td->x->x->id);
            // must recalculate the auxiliary variables for both columns
            int temp = 0, zeroMAD = 0;
            if (nNAx - nNAentriesX[i] > maxDiffNA)
            {
               // Rprintf("...Recalculating row... \n");
               //if (robustX && (fbx!=4))
                  prepareColBicor(xx, nr, maxPOutliers, fbx, xxx, &temp, &NAx, &zeroMAD, xx2, yy2);
               //else
               //   prepareColCor(xx, nr, xxx, &temp, &NAx);
               xx3 = xxx;
            } else
               xx3 = multMatX + i * nr;
            if (nNAy-nNAentriesY[j] > maxDiffNA)
            {
               // Rprintf("...Recalculating column... \n");
               //if (robustY && (fby!=4))
                  prepareColBicor(yy, nr, maxPOutliers, fby, yyy, &temp, &NAy, &zeroMAD, xx2, yy2);
               //else
               //   prepareColCor(yy, nr, yyy, &temp, &NAy);
               yy3 = yyy;
            } else
               yy3 = multMatY + j * nr;
            if (NAx + NAy==0)
            {
               // LDOUBLE sumxy = 0;
               double sumxy = 0;
               int count = 0;
               for (int k=0; k<nr; k++)
               {
                 double vx = *(xx3 + k), vy = *(yy3 +  k);
                 // Rprintf("i: %d, j: %d, k: %d: vx: %e, vy: %e\n", i,j,k,vx,vy);
                 if (!ISNAN(vx) && !ISNAN(vy))
                 {
                   sumxy += vx * vy; 
                   count++;
                 }
               }
               if (count==0) 
               {
                  result[i + j*ncx] = NA_REAL; 
                  (*nNA)++;
               } else {
                  result[i + j*ncx] = (double) sumxy;
                  // Rprintf("Recalculated row %d and column %d, column size %d in thread %d: result = %7.4f\n", 
                  //         i, j, nr, td->x->x->id,  result[i + j*ncx]);
               }
            } else {
                result[i + j*ncx] = NA_REAL; 
                (*nNA)++;
            }
            (*nSlow)++;
        }
     }
  }
  return NULL;
}



//===================================================================================================
//
// Two-variable bicorrelation. Basically the same as bicor1, just must calculate the whole matrix.
// If robustX,Y is zero, the corresponding variable will be treated as in pearson correlation.
//
//===================================================================================================

void bicorFast(double * x, int * nrow, int * ncolx, double * y, int * ncoly,
           int * robustX, int * robustY, double *maxPOutliers, 
           double * quick, int * fallback,
           double * result, int *nNA, int * err,
           int * nThreads,
           int * verbose, int * indent)
{
  int nr = *nrow, ncx = *ncolx, ncy = *ncoly;

  char          spaces[2* *indent+1];
  for (int i=0; i<2* *indent; i++) spaces[i] = ' ';
  spaces[2* *indent] = '\0';

  *nNA = 0;

  double * multMatX, * multMatY;
  int * nNAentriesX, * nNAentriesY;
  int *NAmedX, *NAmedY;

  if ( (multMatX = malloc(ncx*nr * sizeof(double)))==NULL )
  {
    *err = 1;
    Rprintf("bicor: memmory allocation error. If possible, please decrease block size.\n");
    return;
  }

  if ( (multMatY = malloc(ncy*nr * sizeof(double)))==NULL )
  {
    free(multMatX);
    *err = 1;
    Rprintf("bicor: memmory allocation error. If possible, please decrease block size.\n");
    return;
  }

  if ( (nNAentriesX = malloc(ncx * sizeof(int)))==NULL )
  {
    free(multMatY); free(multMatX);
    *err = 1;
    Rprintf("bicor: memmory allocation error. The needed block is relatively small... suspicious.\n");
    return;
  }

  if ( (nNAentriesY = malloc(ncy * sizeof(int)))==NULL )
  {
    free(nNAentriesX); free(multMatY); free(multMatX);
    *err = 1;
    Rprintf("bicor: memmory allocation error. The needed block is relatively small... suspicious.\n");
    return;
  }

  if ( (NAmedX = malloc(ncx * sizeof(int)))==NULL )
  {
    free(nNAentriesY); free(nNAentriesX); free(multMatY); free(multMatX);
    *err = 1;
    Rprintf("bicor: memmory allocation error. The needed block is relatively small... suspicious.\n");
    return;
  }

  if ( (NAmedY = malloc(ncy * sizeof(int)))==NULL )
  {
    free(NAmedX); free(nNAentriesY); free(nNAentriesX); free(multMatY); free(multMatX);
    *err = 1;
    Rprintf("bicor: memmory allocation error. The needed block is relatively small... suspicious.\n");
    return;
  }

  int nt = *nThreads;
  if ((nt < 1) || (nt > MxThreads))
  {
    nt = nProcessors();
    if (nt >MxThreads) nt = MxThreads;
  }
  if (*verbose)
      Rprintf("%s..will use %d parallel threads.\n", spaces, nt);

  double * aux[MxThreads];

  for (int t=0; t < nt; t++)
  {
     if ( (aux[t] = malloc(6*nr * sizeof(double)))==NULL)
     {
       *err = 1;
       Rprintf("cor1: memmory allocation error. The needed block is very small... suspicious.\n");
       for (int tt = t-1; tt>=0; tt--) free(aux[tt]);
       free(NAmedY); free(NAmedX); free(nNAentriesY); free(nNAentriesX); free(multMatY); free(multMatX);
       return;
     }
  }

  cor1ThreadData thrdInfoX[MxThreads];
  cor1ThreadData thrdInfoY[MxThreads];
  cor2ThreadData thrdInfo[MxThreads];

  for (int t = 0; t < nt; t++)
  {
     thrdInfoX[t].x = x;
     thrdInfoX[t].nr = nr;
     thrdInfoX[t].nc = ncx;
     thrdInfoX[t].multMat = multMatX;
     thrdInfoX[t].result = result;
     thrdInfoX[t].nNAentries = nNAentriesX;
     thrdInfoX[t].NAme = NAmedX;
     thrdInfoX[t].aux = aux[t];
     thrdInfoX[t].robust = *robustX;
     thrdInfoX[t].fallback = *fallback;
     thrdInfoX[t].maxPOutliers = *maxPOutliers;
     thrdInfoX[t].quick = *quick;
     thrdInfoX[t].id = t;

   
     thrdInfoY[t].x = y;
     thrdInfoY[t].nr = nr;
     thrdInfoY[t].nc = ncy;
     thrdInfoY[t].multMat = multMatY;
     thrdInfoY[t].result = result;
     thrdInfoY[t].nNAentries = nNAentriesY;
     thrdInfoY[t].NAme = NAmedY;
     thrdInfoY[t].aux = aux[t] + 3 * nr;
     thrdInfoY[t].robust = *robustY;
     thrdInfoY[t].fallback = *fallback;
     thrdInfoY[t].maxPOutliers = *maxPOutliers;
     thrdInfoY[t].quick = *quick;
     thrdInfoY[t].id = t;

     thrdInfo[t].x = thrdInfoX + t;
     thrdInfo[t].y = thrdInfoY + t;
  }

  // Prepare the multMat columns in X and Y

  // Rprintf(" ..preparing columns in x\n");

  colPrepThreadData  cptd[MxThreads];
  pthread_t  thr[MxThreads];
  int       status[MxThreads];

  progressCounter pcX, pcY;
  int pearsonX = 0, pearsonY = 0;

  // Prepare columns in X
 
  pthread_mutex_t mutex1 = PTHREAD_MUTEX_INITIALIZER;

  pcX.i = 0;
  pcX.n = ncx;
  for (int t=0; t<nt; t++)
  {
    cptd[t].x = &thrdInfoX[t];
    cptd[t].pc = &pcX;
    cptd[t].lock = &mutex1;
    if (* robustX)
         status[t] = pthread_create(&thr[t], NULL, threadPrepColBicor, (void *) &cptd[t]);
       else
         status[t] = pthread_create(&thr[t], NULL, threadPrepColCor, (void *) &cptd[t]);
  }
  for (int t=0; t<nt; t++)
      pthread_join(thr[t], NULL);

  // If the fallback method is to re-do everything in Pearson, check whether any columns had zero MAD.
  if (*fallback==3)
  {
    for (int t=0; t<nt; t++) if (thrdInfoX[t].zeroMAD > 0)
    { 
      pearsonX = 1;
      if (*verbose)
        Rprintf("Warning in bicor(x, y): MAD is zero in column %d of x. %s", 
                thrdInfoX[t].zeroMAD, "Switching to Pearson calculation for x.\n");
    }
    if (pearsonX==1) // Re-do all column preparations 
    { 
      for (int t = 0; t < nt; t++) thrdInfoX[t].fallback = 4;

      pthread_mutex_t mutex2 = PTHREAD_MUTEX_INITIALIZER;
      pcX.i = 0;
      pcX.n = ncx;

      for (int t=0; t<nt; t++)
      {
        cptd[t].lock = &mutex2;
        status[t] = pthread_create(&thr[t], NULL, threadPrepColCor, (void *) &cptd[t]);
      }
      for (int t=0; t<nt; t++) pthread_join(thr[t], NULL);
    }
  }


  // Prepare columns in Y
 
  // Rprintf(" ..preparing columns in y\n");
  pthread_mutex_t mutex1Y = PTHREAD_MUTEX_INITIALIZER;

  pcY.i = 0;
  pcY.n = ncy;
  for (int t=0; t<nt; t++)
  {
    cptd[t].x = &thrdInfoY[t];
    cptd[t].pc = &pcY;
    cptd[t].lock = &mutex1Y;
    if (* robustY)
         status[t] = pthread_create(&thr[t], NULL, threadPrepColBicor, (void *) &cptd[t]);
       else
         status[t] = pthread_create(&thr[t], NULL, threadPrepColCor, (void *) &cptd[t]);
  }

  for (int t=0; t<nt; t++)
      pthread_join(thr[t], NULL);

  // If the fallback method is to re-do everything in Pearson, check whether any columns had zero MAD.
  if (*fallback==3)
  {
    for (int t=0; t<nt; t++) if (thrdInfoY[t].zeroMAD > 0)
    { 
      pearsonY = 1;
      if (*verbose)
        Rprintf("Warning in bicor(x, y): MAD is zero in column %d of x. %s", 
                thrdInfoY[t].zeroMAD, "Switching to Pearson calculation for x.\n");
    }
    if (pearsonY==1) // Re-do all column preparations 
    { 
      for (int t = 0; t < nt; t++) thrdInfoY[t].fallback = 4;

      pthread_mutex_t mutex2Y = PTHREAD_MUTEX_INITIALIZER;
      pcY.i = 0;
      pcY.n = ncx;

      for (int t=0; t<nt; t++)
      {
        cptd[t].lock = &mutex2Y;
        status[t] = pthread_create(&thr[t], NULL, threadPrepColCor, (void *) &cptd[t]);
      }
      for (int t=0; t<nt; t++) pthread_join(thr[t], NULL);
    }
  }

  // Rprintf("multMatX:\n");
  // for (int i=0; i<nr; i++)
  // {
  //   for (int j=0; j<ncx; j++) Rprintf(" %7.4f ", multMatX[i + nr*j]);
  //   Rprintf("\n");
 //  }
 
  // Rprintf("multMatY:\n");
 //  for (int i=0; i<nr; i++)
 //  {
   //  for (int j=0; j<ncy; j++) Rprintf(" %7.4f ", multMatY[i + nr*j]);
   //  Rprintf("\n");
 //  }

   // Rprintf("nNAentriesX:");
   // for (int i=0; i<ncx; i++) Rprintf(" %d,", nNAentriesX[i]);
   // Rprintf("\n");
   // Rprintf("nNAentriesY:");
   // for (int i=0; i<ncx; i++) Rprintf(" %d,", nNAentriesY[i]);
   // Rprintf("\n");
 

  // The main calculation: matrix multiplication
  
  double alpha = 1.0, beta = 1.0;
  dgemm_("T", "N", ncolx, ncoly, nrow, & alpha, multMatX, nrow, multMatY, nrow, & beta, result, ncolx);

  // Rprintf("matrix multiplication result:\n");
  // for (int i=0; i<ncx; i++)
  // {
  //   for (int j=0; j<ncy; j++) Rprintf(" %7.4f ", result[i + ncx*j]);
  //   Rprintf("\n");
 //  }


  // Remedial calculations

  int nSlow = 0;
  if (*quick < 1.0)
  {
      slowCalc2ThreadData  sctd[MxThreads];
      pthread_mutex_t mutexSC = PTHREAD_MUTEX_INITIALIZER;

      pthread_t  thr3[MxThreads];

      pcX.i = 0; pcY.i = 0;

      // Rprintf("slow calculations... nt=%d\n", nt);
      for (int t=0; t<nt; t++)
      {
        sctd[t].x = &thrdInfo[t];
        sctd[t].pci = &pcX;
        sctd[t].pcj = &pcY;
        sctd[t].nSlow = &nSlow;
        sctd[t].nNA = nNA;
        sctd[t].lock = &mutexSC;
        sctd[t].quick = *quick;
        status[t] = pthread_create(&thr3[t], NULL, threadSlowCalcBicor2, (void *) &sctd[t]);
      }

      for (int t=0; t<nt; t++)
          pthread_join(thr3[t], NULL);

      if (*verbose) Rprintf("%s Fraction of slow calculations: %f\n", spaces,
                             ( (double) nSlow) / (ncx*ncy) );
  }

  // NA out all rows and columns that need it and check for values outside of [-1, 1]

  NA2ThreadData  natd[MxThreads];
  // reset the progress counter
  pcX.i = 0;
  pcY.i = 0;

  pthread_t  thr2[MxThreads];

  for (int t=0; t<nt; t++)
  {
    natd[t].x = &thrdInfo[t];
    natd[t].pci = &pcX;
    natd[t].pcj = &pcY;
    status[t] = pthread_create(&thr2[t], NULL, threadNAing, (void *) &natd[t]);
  }

  for (int t=0; t<nt; t++)
      pthread_join(thr2[t], NULL);

  // Clean up

  for (int t=nt-1; t >= 0; t--) free(aux[t]);
  free(NAmedY);
  free(NAmedX);
  free(nNAentriesY);
  free(nNAentriesX);
  free(multMatY);
  free(multMatX);
}



/*===================================================================================================
 *
 * Threaded "slow" calculations for pearson correlation of 2 variables.
 *
 *===================================================================================================
*/
// This can actually be relatively slow, since the search for calculations that need to be done is not
// parallel, so one thread may have to traverse the whole matrix. I can imagine parallelizing even that
// part, but for now leave it as is as this will at best be a minuscule improvement.

void * threadSlowCalcCor2(void * par)
{
 
  slowCalc2ThreadData * td = (slowCalc2ThreadData *) par;
  int * nSlow = td->nSlow;
  int * nNA = td->nNA;

  double * x = td->x->x->x;
  double * multMatX = td->x->x->multMat;
  double * result = td->x->x->result;
  int ncx = td->x->x->nc, nr = td->x->x->nr;
  int * NAmeanX = td->x->x->NAme;
  int * nNAentriesX = td->x->x->nNAentries;

  double * y = td->x->y->x;
  double * multMatY = td->x->y->multMat;
  int ncy = td->x->y->nc;
  int * NAmeanY = td->x->y->NAme;
  int * nNAentriesY = td->x->y->nNAentries;

  int maxDiffNA = (int) (td->x->x->quick * nr);

  progressCounter * pci = td->pci, * pcj = td->pcj;

  double * xx, * yy;
  double vx = 0, vy = 0;


  // Rprintf("Will tolerate %d additional NAs\n",  maxDiffNA);
  // Rprintf("Checking %d rows and %d columns\n", nc1, nc);
  // Rprintf("starting at %d and %d\n", pci->i, pcj->i);
  //

  while (pci->i < ncx)
  {
     pthread_mutex_lock( td->lock );
     int i = pci->i, ii = i;
     int j = pcj->i, jj = j;
     do
     {
       i = ii;
       j = jj;
       jj++;
       if (jj==ncy) 
       {
         ii++;
         jj = 0;
       }
     } while ((i<ncx) && (j<ncy) && 
              ((NAmeanX[i] > 0) || (NAmeanY[j] > 0) || 
                ( (nNAentriesX[i] <= maxDiffNA) && ( nNAentriesY[j] <= maxDiffNA))));
     pci->i = ii;
     pcj->i = jj;
     pthread_mutex_unlock( td->lock );
 
     if ((i < ncx) && (j < ncy))
     {
        // Rprintf("Recalculating row %d and column %d, column size %d\n", i, j, nr);
        xx = x + i * nr; yy = y + j * nr;
        LDOUBLE sumxy = 0, sumx = 0, sumy = 0, sumxs = 0, sumys = 0;
        int count = 0;
       
        for (int k=0; k<nr; k++)
        {
           vx = *xx; vy = *yy;
           if (!ISNAN(vx) && !ISNAN(vy))
           {
             count ++;
             sumxy += vx * vy;
             sumx += vx;
             sumy += vy;
             sumxs += vx*vx;
             sumys += vy*vy;
           }
           xx++; yy++;
        }
        if (count==0) 
        {
            result[i + j*ncx] = NA_REAL; 
            (*nNA)++;
        } else {
            result[i + j*ncx] = (double) ( (sumxy - sumx * sumy/count)/
                                sqrtl( (sumxs - sumx*sumx/count) * (sumys - sumy*sumy/count) ) );
        }
        (*nSlow)++;
     }
  }
  return NULL;
}



//===================================================================================================
//
// Two-variable Pearson correlation. 
//
//===================================================================================================

void corFast(double * x, int * nrow, int * ncolx, double * y, int * ncoly,
           double * quick, double * result, int *nNA, int * err,
           int * nThreads,
           int * verbose, int * indent)
{
  int nr = *nrow, ncx = *ncolx, ncy = *ncoly;

  char          spaces[2* *indent+1];
  for (int i=0; i<2* *indent; i++) spaces[i] = ' ';
  spaces[2* *indent] = '\0';

  *nNA = 0;

  double * multMatX, * multMatY;
  int * nNAentriesX, * nNAentriesY;
  int *NAmeanX, *NAmeanY;

  if ( (multMatX = malloc(ncx*nr * sizeof(double)))==NULL )
  {
    *err = 1;
    Rprintf("bicor: memmory allocation error. If possible, please decrease block size.\n");
    return;
  }

  if ( (multMatY = malloc(ncy*nr * sizeof(double)))==NULL )
  {
    free(multMatX);
    *err = 1;
    Rprintf("bicor: memmory allocation error. If possible, please decrease block size.\n");
    return;
  }

  if ( (nNAentriesX = malloc(ncx * sizeof(int)))==NULL )
  {
    free(multMatY); free(multMatX);
    *err = 1;
    Rprintf("bicor: memmory allocation error. The needed block is relatively small... suspicious.\n");
    return;
  }

  if ( (nNAentriesY = malloc(ncy * sizeof(int)))==NULL )
  {
    free(nNAentriesX); free(multMatY); free(multMatX);
    *err = 1;
    Rprintf("bicor: memmory allocation error. The needed block is relatively small... suspicious.\n");
    return;
  }

  if ( (NAmeanX = malloc(ncx * sizeof(int)))==NULL )
  {
    free(nNAentriesY); free(nNAentriesX); free(multMatY); free(multMatX);
    *err = 1;
    Rprintf("bicor: memmory allocation error. The needed block is relatively small... suspicious.\n");
    return;
  }

  if ( (NAmeanY = malloc(ncy * sizeof(int)))==NULL )
  {
    free(NAmeanX); free(nNAentriesY); free(nNAentriesX); free(multMatY); free(multMatX);
    *err = 1;
    Rprintf("bicor: memmory allocation error. The needed block is relatively small... suspicious.\n");
    return;
  }

  int nt = *nThreads;
  if ((nt < 1) || (nt > MxThreads))
  {
    nt = nProcessors();
    if (nt >MxThreads) nt = MxThreads;
  }
  if (*verbose)
      Rprintf("%s..will use %d parallel threads.\n", spaces, nt);

  cor1ThreadData thrdInfoX[MxThreads];
  cor1ThreadData thrdInfoY[MxThreads];
  cor2ThreadData thrdInfo[MxThreads];

  for (int t = 0; t < nt; t++)
  {
     thrdInfoX[t].x = x;
     thrdInfoX[t].nr = nr;
     thrdInfoX[t].nc = ncx;
     thrdInfoX[t].multMat = multMatX;
     thrdInfoX[t].result = result;
     thrdInfoX[t].nNAentries = nNAentriesX;
     thrdInfoX[t].NAme = NAmeanX;
     thrdInfoX[t].quick = *quick;
     thrdInfoX[t].maxPOutliers = 1;
     thrdInfoX[t].id = t;
   
     thrdInfoY[t].x = y;
     thrdInfoY[t].nr = nr;
     thrdInfoY[t].nc = ncy;
     thrdInfoY[t].multMat = multMatY;
     thrdInfoY[t].result = result;
     thrdInfoY[t].nNAentries = nNAentriesY;
     thrdInfoY[t].NAme = NAmeanY;
     thrdInfoY[t].quick = *quick;
     thrdInfoY[t].maxPOutliers = 1;
     thrdInfoY[t].id = t;

     thrdInfo[t].x = thrdInfoX + t;
     thrdInfo[t].y = thrdInfoY + t;
  }

  // Prepare the multMat columns in X and Y

  colPrepThreadData  cptd[MxThreads];
  pthread_t  thr[MxThreads];
  int       status[MxThreads];

  progressCounter pcX, pcY;

  // Prepare columns in X
 
  pthread_mutex_t mutex1 = PTHREAD_MUTEX_INITIALIZER;

  pcX.i = 0;
  pcX.n = ncx;
  for (int t=0; t<nt; t++)
  {
    cptd[t].x = &thrdInfoX[t];
    cptd[t].pc = &pcX;
    cptd[t].lock = &mutex1;
    status[t] = pthread_create(&thr[t], NULL, threadPrepColCor, (void *) &cptd[t]);
  }
  for (int t=0; t<nt; t++)
      pthread_join(thr[t], NULL);

  // Prepare columns in Y
 
  pthread_mutex_t mutex1Y = PTHREAD_MUTEX_INITIALIZER;

  pcY.i = 0;
  pcY.n = ncy;
  for (int t=0; t<nt; t++)
  {
    cptd[t].x = &thrdInfoY[t];
    cptd[t].pc = &pcY;
    cptd[t].lock = &mutex1Y;
    status[t] = pthread_create(&thr[t], NULL, threadPrepColCor, (void *) &cptd[t]);
  }

  for (int t=0; t<nt; t++)
      pthread_join(thr[t], NULL);

  //Rprintf("multMatX:\n");
  //for (int i=0; i<nr; i++)
  //{
    //for (int j=0; j<ncx; j++) Rprintf(" %7.4f ", multMatX[i + nr*j]);
    //Rprintf("\n");
  //}
 
  //Rprintf("multMatY:\n");
  //for (int i=0; i<nr; i++)
  //{
    //for (int j=0; j<ncy; j++) Rprintf(" %7.4f ", multMatY[i + nr*j]);
    //Rprintf("\n");
  //}

  // Rprintf("nNAentriesX:");
  // for (int i=0; i<ncx; i++) Rprintf(" %d,", nNAentriesX[i]);
  // Rprintf("\n");
  // Rprintf("nNAentriesY:");
  // for (int i=0; i<ncx; i++) Rprintf(" %d,", nNAentriesY[i]);
  // Rprintf("\n");
 

  // The main calculation: matrix multiplication
  
  double alpha = 1.0, beta = 1.0;
  dgemm_("T", "N", ncolx, ncoly, nrow, & alpha, multMatX, nrow, multMatY, nrow, & beta, result, ncolx);

  // Remedial calculations

  int nSlow = 0;
  if (*quick < 1.0)
  {
      slowCalc2ThreadData  sctd[MxThreads];
      pthread_mutex_t mutexSC = PTHREAD_MUTEX_INITIALIZER;

      pthread_t  thr3[MxThreads];

      pcX.i = 0; pcY.i = 0;

      // Rprintf("slow calculations... nt=%d\n", nt);
      for (int t=0; t<nt; t++)
      {
        sctd[t].x = &thrdInfo[t];
        sctd[t].pci = &pcX;
        sctd[t].pcj = &pcY;
        sctd[t].nSlow = &nSlow;
        sctd[t].nNA = nNA;
        sctd[t].lock = &mutexSC;
        sctd[t].quick = *quick;
        status[t] = pthread_create(&thr3[t], NULL, threadSlowCalcCor2, (void *) &sctd[t]);
      }

      for (int t=0; t<nt; t++)
          pthread_join(thr3[t], NULL);

      if (*verbose) Rprintf("%s Fraction of slow calculations: %f\n", spaces,
                             ( (double) nSlow) / (ncx*ncy) );
  }

  // NA out all rows and columns that need it and check for values outside of [-1, 1]
  //
  NA2ThreadData  natd[MxThreads];
  // reset the progress counters
  pcX.i = 0;
  pcY.i = 0;

  pthread_t  thr2[MxThreads];

  for (int t=0; t<nt; t++)
  {
    natd[t].x = &thrdInfo[t];
    natd[t].pci = &pcX;
    natd[t].pcj = &pcY;
    status[t] = pthread_create(&thr2[t], NULL, threadNAing, (void *) &natd[t]);
  }

  for (int t=0; t<nt; t++)
      pthread_join(thr2[t], NULL);

  // clean up and return

  free(NAmeanY);
  free(NAmeanX);
  free(nNAentriesY);
  free(nNAentriesX);
  free(multMatY);
  free(multMatX);
}


#endif
