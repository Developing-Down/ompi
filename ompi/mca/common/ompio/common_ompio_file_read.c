/*
 *  Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                          University Research and Technology
 *                          Corporation.  All rights reserved.
 *  Copyright (c) 2004-2016 The University of Tennessee and The University
 *                          of Tennessee Research Foundation.  All rights
 *                          reserved.
 *  Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                          University of Stuttgart.  All rights reserved.
 *  Copyright (c) 2004-2005 The Regents of the University of California.
 *                          All rights reserved.
 *  Copyright (c) 2008-2019 University of Houston. All rights reserved.
 *  Copyright (c) 2018      Research Organization for Information Science
 *                          and Technology (RIST). All rights reserved.
 *  Copyright (c) 2022      Advanced Micro Devices, Inc. All rights reserved.
 *  $COPYRIGHT$
 *
 *  Additional copyrights may follow
 *
 *  $HEADER$
 */

#include "ompi_config.h"

#include "ompi/communicator/communicator.h"
#include "ompi/info/info.h"
#include "ompi/file/file.h"
#include "ompi/mca/fs/fs.h"
#include "ompi/mca/fs/base/base.h"
#include "ompi/mca/fcoll/fcoll.h"
#include "ompi/mca/fcoll/base/base.h"
#include "ompi/mca/fbtl/fbtl.h"
#include "ompi/mca/fbtl/base/base.h"

#include "common_ompio.h"
#include "common_ompio_request.h"
#include "common_ompio_buffer.h"
#include <unistd.h>
#include <math.h>


/* Read and write routines are split into two interfaces.
**   The
**   mca_io_ompio_file_read/write[_at]
**
** routines are the ones registered with the ompio modules.
** They call the
**
** mca_common_ompio_file_read/write[_at]
**
** routines, which are however also used from the shared file pointer modules.
** The main difference is, that the first one takes an ompi_file_t
** as a file pointer argument, while the second uses the ompio internal
** ompio_file_t structure.
*/

static int mca_common_ompio_file_read_default (ompio_file_t *fh, void *buf,
                                               int count, struct ompi_datatype_t *datatype,
                                               ompi_status_public_t *status);

static int mca_common_ompio_file_read_pipelined (ompio_file_t *fh, void *buf,
                                                 int count, struct ompi_datatype_t *datatype,
                                                 ompi_status_public_t *status);

int mca_common_ompio_file_read (ompio_file_t *fh,
                              void *buf,
                              int count,
                              struct ompi_datatype_t *datatype,
                              ompi_status_public_t *status)
{
    bool need_to_copy = false;
    int is_gpu, is_managed;

    if (fh->f_amode & MPI_MODE_WRONLY) {
        return MPI_ERR_ACCESS;
    }

    if (0 == count || 0 == fh->f_iov_count) {
        if (MPI_STATUS_IGNORE != status) {
            status->_ucount = 0;
        }
        return OMPI_SUCCESS;
    }

    mca_common_ompio_check_gpu_buf ( fh, buf, &is_gpu, &is_managed);
    if (is_gpu && !is_managed) {
        need_to_copy = true;
    }

    if ( !( fh->f_flags & OMPIO_DATAREP_NATIVE ) &&
         !(datatype == &ompi_mpi_byte.dt  ||
           datatype == &ompi_mpi_char.dt   )) {
        /* only need to copy if any of these conditions are given:
           1. buffer is an unmanaged device buffer (checked above).
           2. Datarepresentation is anything other than 'native' and
           3. datatype is not byte or char (i.e it does require some actual
              work to be done e.g. for external32.
        */
        need_to_copy = true;
    }         

    if (need_to_copy) {
        return mca_common_ompio_file_read_pipelined (fh, buf, count, datatype, status);
    } else {
        return mca_common_ompio_file_read_default (fh, buf, count, datatype, status);
    }

    return OMPI_SUCCESS; //silence compiler
}

int mca_common_ompio_file_read_default (ompio_file_t *fh, void *buf,
                                        int count, struct ompi_datatype_t *datatype,
                                        ompi_status_public_t *status)
{
    size_t total_bytes_read = 0;       /* total bytes that have been read*/
    size_t bytes_per_cycle = 0;        /* total read in each cycle by each process*/
    int index = 0;
    int cycles = 0;

    uint32_t iov_count = 0;
    struct iovec *decoded_iov = NULL;

    size_t max_data=0, real_bytes_read=0;
    size_t spc=0;
    ssize_t ret_code=0;
    int i = 0; /* index into the decoded iovec of the buffer */
    int j = 0; /* index into the file via iovec */

    mca_common_ompio_decode_datatype (fh, datatype, count, buf,
                                      &max_data, fh->f_mem_convertor,
                                      &decoded_iov, &iov_count);

    bytes_per_cycle = OMPIO_MCA_GET(fh, cycle_buffer_size);
    cycles = ceil((double)max_data/bytes_per_cycle);
    
#if 0
    printf ("Bytes per Cycle: %d   Cycles: %d max_data:%d \n", bytes_per_cycle,
            cycles, max_data);
#endif

    j = fh->f_index_in_file_view;
    for (index = 0; index < cycles; index++) {
        mca_common_ompio_build_io_array (fh, index, cycles, bytes_per_cycle,
                                         max_data, iov_count, decoded_iov,
                                         &i, &j, &total_bytes_read, &spc,
                                         &fh->f_io_array, &fh->f_num_of_io_entries);
        if (fh->f_num_of_io_entries == 0) {
	    ret_code = 0;
	    goto exit;
	}

	ret_code = fh->f_fbtl->fbtl_preadv (fh);
	if (0 <= ret_code) {
	    real_bytes_read += (size_t)ret_code;
	    // Reset ret_code since it is also used to return an error
	    ret_code = 0;
	} else {
	    goto exit;
	}

        fh->f_num_of_io_entries = 0;
        free (fh->f_io_array);
        fh->f_io_array = NULL;
    }

 exit:
    free (decoded_iov);
    if (MPI_STATUS_IGNORE != status) {
        status->_ucount = real_bytes_read;
    }

    return ret_code;
}

int mca_common_ompio_file_read_pipelined (ompio_file_t *fh, void *buf,
                                          int count, struct ompi_datatype_t *datatype,
                                          ompi_status_public_t *status)
{
    size_t tbr = 0;               /* total bytes that have been read*/
    size_t bytes_per_cycle = 0;   /* total read in each cycle by each process*/
    size_t bytes_this_cycle = 0, bytes_prev_cycle = 0;
    int index = 0;
    int cycles = 0;
    uint32_t iov_count = 0;
    struct iovec *decoded_iov = NULL;

    size_t max_data=0, real_bytes_read=0;
    size_t spc=0;
    ssize_t ret_code=0;
    int i = 0; /* index into the decoded iovec of the buffer */
    int j = 0; /* index into the file via iovec */

    char *tbuf1=NULL, *tbuf2=NULL;
    char *unpackbuf=NULL, *readbuf=NULL;
    mca_ompio_request_t *ompio_req=NULL, *prev_ompio_req=NULL;
    opal_convertor_t convertor;
    bool can_overlap = (NULL != fh->f_fbtl->fbtl_ipreadv);

    bytes_per_cycle = OMPIO_MCA_GET(fh, pipeline_buffer_size);
    OMPIO_PREPARE_READ_BUF (fh, buf, count, datatype, tbuf1, &convertor,
                            max_data, bytes_per_cycle, decoded_iov, iov_count);
    cycles = ceil((double)max_data/bytes_per_cycle);

    readbuf = unpackbuf = tbuf1;
    if (can_overlap) {
        tbuf2 = mca_common_ompio_alloc_buf (fh, bytes_per_cycle);
        if (NULL == tbuf2) {
            opal_output(1, "common_ompio: error allocating memory\n");
            free (decoded_iov);
            return OMPI_ERR_OUT_OF_RESOURCE;
        }
        unpackbuf = tbuf2;
    }

#if 0
    printf ("Bytes per Cycle: %d Cycles: %d max_data:%d \n", bytes_per_cycle,
            cycles, max_data);
#endif

    /*
    ** The code combines two scenarios:
    ** 1. having async read (i.e. ipreadv) which allows to overlap two 
    **    iterations.
    ** 2. not having async read, which doesn't allow for overlap.
    ** 
    ** In the first case we use a double buffering technique, the sequence is
    **    - construct io-array for iter i 
    **    - post ipreadv for iter i
    **    - wait for iter i-1
    **    - unpack buffer i-1
    **    - swap buffers
    **
    ** In the second case, the sequence is
    **    - construct io-array for iter i
    **    - post preadv for iter i
    **    - unpack buffer i
    */
    
    j = fh->f_index_in_file_view;
    if (can_overlap) {
	mca_common_ompio_register_progress ();	    
    }

    for (index = 0; index < cycles+1; index++) {
        if (index < cycles) {
            decoded_iov->iov_base = readbuf;
            decoded_iov->iov_len  = bytes_per_cycle;
            bytes_this_cycle      = (index == cycles-1) ?
                (max_data - (index * bytes_per_cycle)) :
                bytes_per_cycle;

	    i   = 0;
            spc = 0;
            tbr = 0;

            mca_common_ompio_build_io_array (fh, index, cycles, bytes_per_cycle,
                                             bytes_this_cycle, iov_count,
                                             decoded_iov, &i, &j, &tbr, &spc,
                                             &fh->f_io_array, &fh->f_num_of_io_entries);
	    if (fh->f_num_of_io_entries == 0) {
		ret_code = 0;
		goto exit;
	    }

            if (can_overlap) {
                mca_common_ompio_request_alloc ( &ompio_req, MCA_OMPIO_REQUEST_READ);
		fh->f_fbtl->fbtl_ipreadv (fh, (ompi_request_t *)ompio_req);
            } else {
		ret_code = fh->f_fbtl->fbtl_preadv (fh);
		if (0 <= ret_code) {
		    real_bytes_read+=(size_t)ret_code;
		    // Reset ret_code since it is also used to return an error
		    ret_code = 0;
		}
		else {
		    goto exit;
		}
	    }
	}

	if (can_overlap) {
	    if (index != 0) {
		ompi_status_public_t stat;
		ret_code = ompi_request_wait ((ompi_request_t **)&prev_ompio_req, &stat);
		if (OMPI_SUCCESS != ret_code) {
		    goto exit;
		}
		real_bytes_read += stat._ucount;
	    }
	    prev_ompio_req = ompio_req;
	}

	if ((can_overlap && index != 0) ||
	    (!can_overlap && index < cycles)) {
	    size_t pos = 0;
	    decoded_iov->iov_base = unpackbuf;
	    decoded_iov->iov_len  = can_overlap ? bytes_prev_cycle : bytes_this_cycle;
	    opal_convertor_unpack (&convertor, decoded_iov, &iov_count, &pos);
	}

	fh->f_num_of_io_entries = 0;
	free (fh->f_io_array);
	fh->f_io_array = NULL;
        bytes_prev_cycle = bytes_this_cycle;

        if (can_overlap) {
            char *tmp = unpackbuf;
            unpackbuf = readbuf;
            readbuf   = tmp;
        }
    }

 exit:
    opal_convertor_cleanup (&convertor);
    mca_common_ompio_release_buf (fh, tbuf1);
    if (can_overlap) {
        mca_common_ompio_release_buf (fh, tbuf2);
    }

    free (decoded_iov);
    if ( MPI_STATUS_IGNORE != status ) {
        status->_ucount = real_bytes_read;
    }

    return ret_code;
}

int mca_common_ompio_file_read_at (ompio_file_t *fh,
				 OMPI_MPI_OFFSET_TYPE offset,
				 void *buf,
				 int count,
				 struct ompi_datatype_t *datatype,
				 ompi_status_public_t * status)
{
    int ret = OMPI_SUCCESS;
    OMPI_MPI_OFFSET_TYPE prev_offset;

    mca_common_ompio_file_get_position (fh, &prev_offset );

    mca_common_ompio_set_explicit_offset (fh, offset);
    ret = mca_common_ompio_file_read (fh,
				    buf,
				    count,
				    datatype,
				    status);

    // An explicit offset file operation is not suppsed to modify
    // the internal file pointer. So reset the pointer
    // to the previous value
    mca_common_ompio_set_explicit_offset (fh, prev_offset);

    return ret;
}


int mca_common_ompio_file_iread (ompio_file_t *fh,
			       void *buf,
			       int count,
			       struct ompi_datatype_t *datatype,
			       ompi_request_t **request)
{
    int ret = OMPI_SUCCESS;
    mca_ompio_request_t *ompio_req=NULL;
    size_t spc=0;

    if (fh->f_amode & MPI_MODE_WRONLY){
//      opal_output(10, "Improper use of FILE Mode, Using WRONLY for Read!\n");
        ret = MPI_ERR_ACCESS;
      return ret;
    }

    mca_common_ompio_request_alloc ( &ompio_req, MCA_OMPIO_REQUEST_READ);

    if ( 0 == count ) {
        ompio_req->req_ompi.req_status.MPI_ERROR = OMPI_SUCCESS;
        ompio_req->req_ompi.req_status._ucount = 0;
        ompi_request_complete (&ompio_req->req_ompi, false);
        *request = (ompi_request_t *) ompio_req;
        
        return OMPI_SUCCESS;
    }

    if ( NULL != fh->f_fbtl->fbtl_ipreadv ) {
        // This fbtl has support for non-blocking operations

        size_t total_bytes_read = 0;       /* total bytes that have been read*/
        uint32_t iov_count = 0;
        struct iovec *decoded_iov = NULL;
        
        size_t max_data = 0;
        int i = 0; /* index into the decoded iovec of the buffer */
        int j = 0; /* index into the file via iovec */
        
        bool need_to_copy = false;    
    
        int is_gpu, is_managed;
        mca_common_ompio_check_gpu_buf ( fh, buf, &is_gpu, &is_managed);
        if ( is_gpu && !is_managed ) {
            need_to_copy = true;
        }

        if ( !( fh->f_flags & OMPIO_DATAREP_NATIVE ) &&
             !(datatype == &ompi_mpi_byte.dt  ||
               datatype == &ompi_mpi_char.dt   )) {
            /* only need to copy if any of these conditions are given:
               1. buffer is an unmanaged device buffer (checked above).
               2. Datarepresentation is anything other than 'native' and
               3. datatype is not byte or char (i.e it does require some actual
               work to be done e.g. for external32.
            */
            need_to_copy = true;
        }         
        
        if ( need_to_copy ) {
            char *tbuf=NULL;
            
            OMPIO_PREPARE_READ_BUF(fh, buf, count, datatype, tbuf, &ompio_req->req_convertor,
                                   max_data, 0, decoded_iov, iov_count);
            
            ompio_req->req_tbuf = tbuf;
            ompio_req->req_size = max_data;
        }
        else {
            mca_common_ompio_decode_datatype (fh,
                                              datatype,
                                              count,
                                              buf,
                                              &max_data,
                                              fh->f_mem_convertor,
                                              &decoded_iov,
                                              &iov_count);
        }
    
        if ( 0 < max_data && 0 == fh->f_iov_count  ) {
            ompio_req->req_ompi.req_status.MPI_ERROR = OMPI_SUCCESS;
            ompio_req->req_ompi.req_status._ucount = 0;
            ompi_request_complete (&ompio_req->req_ompi, false);
            *request = (ompi_request_t *) ompio_req;
            if (NULL != decoded_iov) {
                free (decoded_iov);
                decoded_iov = NULL;
            }

            return OMPI_SUCCESS;
        }

        // Non-blocking operations have to occur in a single cycle
        j = fh->f_index_in_file_view;
        
        mca_common_ompio_build_io_array ( fh,
                                          0,         // index
                                          1,         // no. of cyces
                                          max_data,  // setting bytes per cycle to match data
                                          max_data,
                                          iov_count,
                                          decoded_iov,
                                          &i,
                                          &j,
                                          &total_bytes_read, 
                                          &spc,
                                          &fh->f_io_array,
                                          &fh->f_num_of_io_entries);

	if (fh->f_num_of_io_entries) {
	  fh->f_fbtl->fbtl_ipreadv (fh, (ompi_request_t *) ompio_req);
	}

        mca_common_ompio_register_progress ();

	fh->f_num_of_io_entries = 0;
	if (NULL != fh->f_io_array) {
	    free (fh->f_io_array);
	    fh->f_io_array = NULL;
	}

	if (NULL != decoded_iov) {
	    free (decoded_iov);
	    decoded_iov = NULL;
	}
    }
    else {
	// This fbtl does not  support non-blocking operations
	ompi_status_public_t status;
	ret = mca_common_ompio_file_read (fh, buf, count, datatype, &status);

	ompio_req->req_ompi.req_status.MPI_ERROR = ret;
	ompio_req->req_ompi.req_status._ucount = status._ucount;
	ompi_request_complete (&ompio_req->req_ompi, false);
    }

    *request = (ompi_request_t *) ompio_req;
    return ret;
}


int mca_common_ompio_file_iread_at (ompio_file_t *fh,
				  OMPI_MPI_OFFSET_TYPE offset,
				  void *buf,
				  int count,
				  struct ompi_datatype_t *datatype,
				  ompi_request_t **request)
{
    int ret = OMPI_SUCCESS;
    OMPI_MPI_OFFSET_TYPE prev_offset;
    mca_common_ompio_file_get_position (fh, &prev_offset );

    mca_common_ompio_set_explicit_offset (fh, offset);
    ret = mca_common_ompio_file_iread (fh,
				    buf,
				    count,
				    datatype,
				    request);

    /* An explicit offset file operation is not suppsed to modify
    ** the internal file pointer. So reset the pointer
    ** to the previous value
    ** It is OK to reset the position already here, althgouth
    ** the operation might still be pending/ongoing, since
    ** the entire array of <offset, length, memaddress> have
    ** already been constructed in the file_iread operation
    */
    mca_common_ompio_set_explicit_offset (fh, prev_offset);

    return ret;
}


/* Infrastructure for collective operations  */
int mca_common_ompio_file_read_all (ompio_file_t *fh,
                                    void *buf,
                                    int count,
                                    struct ompi_datatype_t *datatype,
                                    ompi_status_public_t * status)
{
    int ret = OMPI_SUCCESS;


    if ( !( fh->f_flags & OMPIO_DATAREP_NATIVE ) &&
         !(datatype == &ompi_mpi_byte.dt  ||
           datatype == &ompi_mpi_char.dt   )) {
        /* No need to check for GPU buffer for collective I/O.
           Most algorithms copy data from aggregators, and send/recv
           to/from GPU buffers works if ompi was compiled was GPU support.
           
           If the individual fcoll component is used: there are no aggregators 
           in that concept. However, since they call common_ompio_file_write, 
           device buffers are handled by that routine.

           Thus, we only check for
           1. Datarepresentation is anything other than 'native' and
           2. datatype is not byte or char (i.e it does require some actual
              work to be done e.g. for external32.
        */
        size_t pos=0, max_data=0;
        char *tbuf=NULL;
        opal_convertor_t convertor;
        struct iovec *decoded_iov = NULL;
        uint32_t iov_count = 0;

        OMPIO_PREPARE_READ_BUF(fh, buf, count, datatype, tbuf, &convertor,
                               max_data, 0, decoded_iov, iov_count);
        ret = fh->f_fcoll->fcoll_file_read_all (fh,
                                                decoded_iov->iov_base,
                                                decoded_iov->iov_len,
                                                MPI_BYTE,
                                                status);
        opal_convertor_unpack (&convertor, decoded_iov, &iov_count, &pos );

        opal_convertor_cleanup (&convertor);
        mca_common_ompio_release_buf (fh, decoded_iov->iov_base);
        if (NULL != decoded_iov) {
            free (decoded_iov);
            decoded_iov = NULL;
        }
    }
    else {
        ret = fh->f_fcoll->fcoll_file_read_all (fh,
                                                buf,
                                                count,
                                                datatype,
                                                status);
    }
    return ret;
}

int mca_common_ompio_file_read_at_all (ompio_file_t *fh,
				     OMPI_MPI_OFFSET_TYPE offset,
				     void *buf,
				     int count,
				     struct ompi_datatype_t *datatype,
				     ompi_status_public_t * status)
{
    int ret = OMPI_SUCCESS;
    OMPI_MPI_OFFSET_TYPE prev_offset;
    mca_common_ompio_file_get_position (fh, &prev_offset );

    mca_common_ompio_set_explicit_offset (fh, offset);
    ret = mca_common_ompio_file_read_all (fh,
                                          buf,
                                          count,
                                          datatype,
                                          status);
    
    mca_common_ompio_set_explicit_offset (fh, prev_offset);
    return ret;
}

int mca_common_ompio_file_iread_all (ompio_file_t *fp,
                                     void *buf,
                                     int count,
                                     struct ompi_datatype_t *datatype,
                                     ompi_request_t **request)
{
    int ret = OMPI_SUCCESS;

    if ( NULL != fp->f_fcoll->fcoll_file_iread_all ) {
	ret = fp->f_fcoll->fcoll_file_iread_all (fp,
						 buf,
						 count,
						 datatype,
						 request);
    }
    else {
	/* this fcoll component does not support non-blocking
	   collective I/O operations. WE fake it with
	   individual non-blocking I/O operations. */
	ret = mca_common_ompio_file_iread ( fp, buf, count, datatype, request );
    }

    return ret;
}

int mca_common_ompio_file_iread_at_all (ompio_file_t *fp,
				      OMPI_MPI_OFFSET_TYPE offset,
				      void *buf,
				      int count,
				      struct ompi_datatype_t *datatype,
				      ompi_request_t **request)
{
    int ret = OMPI_SUCCESS;
    OMPI_MPI_OFFSET_TYPE prev_offset;

    mca_common_ompio_file_get_position (fp, &prev_offset );
    mca_common_ompio_set_explicit_offset (fp, offset);

    ret = mca_common_ompio_file_iread_all (fp,
                                           buf,
                                           count,
                                           datatype,
                                           request);
    
    mca_common_ompio_set_explicit_offset (fp, prev_offset);
    return ret;
}


int mca_common_ompio_set_explicit_offset (ompio_file_t *fh,
                                          OMPI_MPI_OFFSET_TYPE offset)
{
    size_t i = 0;
    size_t k = 0;

    if ( fh->f_view_size  > 0 ) {
	/* starting offset of the current copy of the filew view */
	fh->f_offset = (fh->f_view_extent *
			((offset*fh->f_etype_size) / fh->f_view_size)) + fh->f_disp;


	/* number of bytes used within the current copy of the file view */
	fh->f_total_bytes = (offset*fh->f_etype_size) % fh->f_view_size;
	i = fh->f_total_bytes;


	/* Initialize the block id and the starting offset of the current block
	   within the current copy of the file view to zero */
	fh->f_index_in_file_view = 0;
	fh->f_position_in_file_view = 0;

	/* determine block id that the offset is located in and
	   the starting offset of that block */
	k = fh->f_decoded_iov[fh->f_index_in_file_view].iov_len;
	while (i >= k) {
	    fh->f_position_in_file_view = k;
	    fh->f_index_in_file_view++;
	    k += fh->f_decoded_iov[fh->f_index_in_file_view].iov_len;
	}
    }

    return OMPI_SUCCESS;
}
