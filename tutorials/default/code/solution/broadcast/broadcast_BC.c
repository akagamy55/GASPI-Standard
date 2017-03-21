#include <stdlib.h>
#include <GASPI.h>
#include <mpi.h>
#include "assert.h"
#include "success_or_die.h"
#include "topology.h"
#include "testsome.h"
#include "constant.h"
#include "now.h"
#include "queue.h"


/* initialize local data */
static void init_array(int *array
		       , int size
		       , gaspi_rank_t iProc
		       )
{ 
  int j;
  for (j = 0; j < size; ++j)
    {
      array[j] = -1;
    }
  if (iProc == 0)
    {
      for (j = 0; j < size; ++j)
	{
	  array[j] = 0;
	}
    }
}

/*
 * restrict NWAY dissemination, when dropping 
 * below minimal message size
 */
static void restrict_NWAY(int *NWAY
			  , int mSize
			  , int N_SZ
			  )
{
  if (M_SZ < mSize)
    {
      *NWAY = 7;
    }
  else if (M_SZ < mSize * N_SZ)
    {
      *NWAY = 3;
    }
  else
    {
      *NWAY = 1;
    }
}


static void restrict_nBlocks(int *nBlocks
			     , int *mSize
			     , int *lSize
			     )
{
  if (M_SZ < *mSize * *nBlocks)
    {
      /* reduce number of blocks */
      if (M_SZ > *mSize)
	{
	  *nBlocks = M_SZ / *mSize;
	  *lSize = M_SZ % *mSize;
	  if (*lSize != 0)
	    {
	      (*nBlocks)++;
	    }
	  else
	    {
	      *lSize = *mSize;
	    } 
	}
      else
	{
	  *nBlocks = 1;
	  *lSize = M_SZ;
	}
    }
  else
    {
      /* increase message size */
      *mSize = M_SZ / *nBlocks;
      *lSize = M_SZ % *mSize;
      if (*lSize != 0)
	{
	  (*nBlocks)++;
	}
      else
	{
	  *lSize = *mSize;
	} 
    }
  ASSERT((*nBlocks - 1) * *mSize + *lSize == M_SZ);
}

/* 
 * validate solution
*/
static void validate(int *array
		     , int size
		     )
{
  int i, j;

  for (j = 0; j < size; ++j)
    {
      ASSERT(array[j] == 0);
    }
}

/* 
 * set up targets for NWAY 
 * dissemination 
 */
static void get_next(int nProc
		     , int NWAY
		     , int (*next)[NWAY]
		     )
{
  int j, k, i = 0;
  int rnd, width = 1;

  for (j = 0; j < nProc; j++)
    {
      for (k = 0; k < NWAY; k++)
	{
	  next[j][k] = -1;
	}
    }
  
  while (i < nProc)
    { 
      for (j = 0; j < width; j++)
        {
	  for (k = 0; k < NWAY; k++)
	    {
	      int nx = i+width+NWAY*j+k;
	      int ix = i+j;
	      if (nx < nProc && ix < nProc)
		{
		  next[ix][k] = nx;
		}
	    }
        }
      i += width;
      width *= NWAY;
    }
  
}

static void wait_and_reset(gaspi_segment_id_t segment_id
			   , gaspi_notification_id_t nid
			   , gaspi_notification_t *val
			   )
{
  gaspi_notification_id_t id;
  SUCCESS_OR_DIE(gaspi_notify_waitsome (segment_id
					, nid
					, 1
					, &id
					, GASPI_BLOCK
					));
  ASSERT(nid == id);
  SUCCESS_OR_DIE(gaspi_notify_reset (segment_id
				     , id
				     , val
				     ));     
}	  


int
main (int argc, char *argv[])
{  

  int nProc_MPI, iProc_MPI;
  gaspi_rank_t iProc, nProc;
  MPI_Init(&argc, &argv);
  MPI_Comm_rank (MPI_COMM_WORLD, &iProc_MPI);
  MPI_Comm_size (MPI_COMM_WORLD, &nProc_MPI);

  SUCCESS_OR_DIE (gaspi_proc_init (GASPI_BLOCK));
  SUCCESS_OR_DIE (gaspi_proc_rank (&iProc));
  SUCCESS_OR_DIE (gaspi_proc_num (&nProc));

  ASSERT(iProc == iProc_MPI);
  ASSERT(nProc == nProc_MPI);

  int mSize = 8192, N_SZ = 32;

  /* restric NWAY dissemination */
  int NWAY = 1;
  restrict_NWAY(&NWAY, mSize, N_SZ);

  /* restrict number of blocks */
  int nBlocks = 512, lSize = 0;
  restrict_nBlocks(&nBlocks, &mSize, &lSize);
  
  /* the root of the broadcast */
  int const b_root = 0;
  int j, k, i;

  int next[nProc][NWAY];
  get_next(nProc, NWAY, next);

  const gaspi_segment_id_t segment_id = 0;
  SUCCESS_OR_DIE (gaspi_segment_create ( segment_id
					 , M_SZ * sizeof(int)
					 , GASPI_GROUP_ALL
					 , GASPI_BLOCK
					 , GASPI_ALLOC_DEFAULT
					 )
		  );
  
  gaspi_pointer_t _ptr = NULL;
  SUCCESS_OR_DIE (gaspi_segment_ptr (segment_id, &_ptr));

  gaspi_number_t queue_num;
  SUCCESS_OR_DIE(gaspi_queue_num (&queue_num));

  int *array = (int *) _ptr;
  init_array(array, M_SZ, iProc);

  SUCCESS_OR_DIE (gaspi_barrier (GASPI_GROUP_ALL, GASPI_BLOCK));

  /* GASPI NWAY round-robin broadcast */
  double time = -now();

  for (j = 0; j < nBlocks; ++j)
    {
      gaspi_notification_t val = 1;
      if (iProc != 0)
	{
	  wait_and_reset(segment_id, j, &val);
	}

      for (i = 0; i < NWAY; ++i)
	{	      
	  int target = next[iProc][i];
	  if (target != -1)
	    {
	      int sz = (j == nBlocks-1) ? lSize : mSize;
	      gaspi_notification_id_t notification = j;
	      gaspi_size_t b_size = sz * sizeof(int);
	      gaspi_offset_t b_offset = j * mSize * sizeof(int);
	      write_notify_and_wait ( segment_id
				      , b_offset
				      , (gaspi_rank_t) target
				      , segment_id
				      , b_offset
				      , b_size
				      , notification
				      , val
				      , target % queue_num
				      );	  
	    }
	}
    }

  time += now();
  printf("# BC  : iProc: %4d, size [byte]: %10d, time: %8.6f, total bandwidth [Mbyte/sec]: %8.0f\n"
	 , iProc, M_SZ, time, (double)(M_SZ*sizeof(int))/1024/1024/time); 
  
  validate(array, M_SZ);

  wait_for_flush_queues();

  SUCCESS_OR_DIE (gaspi_barrier (GASPI_GROUP_ALL, GASPI_BLOCK));

  MPI_Finalize();

  SUCCESS_OR_DIE (gaspi_proc_term (GASPI_BLOCK));

  return EXIT_SUCCESS;
}

