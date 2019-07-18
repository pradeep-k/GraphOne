#pragma once
#include "graph_view.h"
#include "communicator.h"

template<class T>
void do_stream_bfs(gview_t<T>* viewh, uint8_t* lstatus, uint8_t* rstatus, Bitmap* bitmap2)
{
    index_t  frontier = 0;
    scopy2d_client_t<T>* snaph = dynamic_cast<scopy2d_client_t<T>*>(viewh);
    Bitmap* bitmap1 = snaph->bitmap_out;
    

    int row_rank, col_rank;
    MPI_Comm_rank(_row_comm, &row_rank);
    MPI_Comm_rank(_col_comm, &col_rank);
    int col_root = row_rank;
    int row_root = col_rank;
    cout << "col rank = " << col_rank << ":" << _rank <<endl;

    do {
        frontier = 0;
        //#pragma omp parallel num_threads(THD_COUNT) reduction(+:frontier)
        {
        sid_t sid;
        vid_t vid;
        uint8_t level = 0;
        degree_t nebr_count = 0;
        degree_t prior_sz = 65536;
        T* local_adjlist = (T*)malloc(prior_sz*sizeof(T));

        //#pragma omp for nowait
        for (vid_t v = 0; v < snaph->v_count; v++) 
        {
            vid = v;// + snaph->v_offset;
            if(false == bitmap1->get_bit(v) || lstatus[vid] == 255) {
                bitmap1->reset_bit(v);
                continue;
            }
            bitmap1->reset_bit(v);

            nebr_count = snaph->get_degree_out(v);
            if (nebr_count > prior_sz) {
                prior_sz = nebr_count;
                free(local_adjlist);
                local_adjlist = (T*)malloc(prior_sz*sizeof(T));
            }

            level = lstatus[vid];
            snaph->get_nebrs_out(v, local_adjlist);

            for (degree_t i = 0; i < nebr_count; ++i) {
                sid = get_nebr(local_adjlist, i) -  snaph->dst_offset;
                if (rstatus[sid] > level + 1) {
                    rstatus[sid] = level + 1;
                    bitmap2->set_bit(sid);//dest
                    ++frontier;
                    //if (level == 0) { cout << "," << sid; }
                }
            }
        }
        }
        //Finalize the iteration
        //rank should be root
        if (col_rank == col_root) {
            MPI_Reduce(MPI_IN_PLACE, bitmap2->get_start(), bitmap2->get_size(),
                      MPI_UINT64_T, MPI_BOR, col_root, _col_comm);
            MPI_Reduce(MPI_IN_PLACE, rstatus, snaph->v_count,
                      MPI_UINT8_T, MPI_MIN, col_root, _col_comm);
        } else {
            MPI_Reduce(bitmap2->get_start(), bitmap2->get_start(), bitmap2->get_size(),
                      MPI_UINT64_T, MPI_BOR, col_root, _col_comm);
            MPI_Reduce(rstatus, rstatus, snaph->v_count,
                      MPI_UINT8_T, MPI_MIN, col_root, _col_comm);
        }

        MPI_Bcast(bitmap2->get_start(), bitmap2->get_size(), MPI_UINT64_T, row_root, _row_comm);
        MPI_Bcast(lstatus, snaph->v_count, MPI_UINT8_T, row_root, _row_comm);
        
        MPI_Allreduce(MPI_IN_PLACE, &frontier, 1, MPI_UINT64_T, MPI_SUM, _analytics_comm);
        //Swap the bitmaps
        bitmap1->swap(bitmap2);
    } while (frontier);
}

template<class T>
void print_bfs_summary(gview_t<T>* viewh, uint8_t* status, vid_t v_count)
{
    scopy2d_client_t<T>* snaph = dynamic_cast<scopy2d_client_t<T>*>(viewh);
    for (int l = 0; l < 10; ++l) {
        uint64_t vid_count = 0;
        //#pragma omp parallel for reduction (+:vid_count) 
        for (vid_t v = 0; v < snaph->v_count; ++v) {
            if (status[v] == l) ++vid_count;
        }
        MPI_Allreduce(MPI_IN_PLACE, &vid_count, 1, MPI_UINT64_T, MPI_SUM, _col_comm);
            
        if (_rank == 1) { 
            cout << " Level = " << l << " count = " << vid_count << endl;
        }
    }
}

template<class T>
void scopy2d_serial_bfs(gview_t<T>* viewh)
{
    cout << " Rank " << _rank <<" SCopy2d Client Started" << endl;
    scopy2d_client_t<T>* sstreamh = dynamic_cast<scopy2d_client_t<T>*>(viewh);
    pgraph_t<T>* pgraph  = sstreamh->pgraph;
    vid_t        v_count = sstreamh->global_vcount;

    scopy2d_client_t<T>* snaph = sstreamh;
    
    int row_rank, col_rank;
    MPI_Comm_rank(_row_comm, &row_rank);
    MPI_Comm_rank(_col_comm, &col_rank);
    int col_root = row_rank;
    int row_root = col_rank;
    
    sid_t    root   = 1;
    Bitmap bitmap(snaph->v_count);
    int update_count = 0;
    
    uint8_t* lstatus = (uint8_t*)malloc(snaph->v_count*sizeof(uint8_t));
    memset(lstatus, 255, snaph->v_count);
    uint8_t* rstatus = lstatus;
    
    if (root >= snaph->v_offset && root < snaph->v_offset + snaph->v_count) {
        lstatus[root - snaph->v_offset] = 0;
    }
    
    if (row_rank != col_rank) {
        rstatus = (uint8_t*)malloc(snaph->v_count*sizeof(uint8_t));
        memset(rstatus, 255, snaph->v_count);
        if (root >= snaph->dst_offset && root < snaph->dst_offset + snaph->v_count) {
            rstatus[root - snaph->dst_offset] = 0;
        }
    }
    
    double start = mywtime();
    double end = 0;
    
    while (pgraph->get_snapshot_marker() < _edge_count) {
        //update the sstream view
        if (eOK != sstreamh->update_view()) {
            usleep(100);
        }
        ++update_count;
        //cout << " Rank " << _rank << " "<< snaph->v_offset << ":" << snaph->v_count << endl;
        //start = mywtime();
	    do_stream_bfs(snaph, lstatus, rstatus, &bitmap);
        //end = mywtime();
        //cout << " BFS Time at Batch " << update_count << " = " << end - start << endl;
    } 
    print_bfs_summary(snaph, lstatus, v_count); 
}

template<class T>
void scopy2d_server(gview_t<T>* viewh)
{
    scopy2d_server_t<T>* sstreamh = dynamic_cast<scopy2d_server_t<T>*>(viewh);
    pgraph_t<T>* pgraph  = sstreamh->pgraph;
    vid_t        v_count = sstreamh->get_vcount();
    
    index_t beg_marker = 0;//(_edge_count >> 1);
    index_t rest_edges = _edge_count - beg_marker;
    index_t do_count = residue;
    index_t batch_size = rest_edges/do_count;
    index_t marker = 0; 
    int update_count = 1;
    
    cout << " SCopy2d Server Started" << endl;
    
    while (pgraph->get_archived_marker() < _edge_count) {
        marker = beg_marker + update_count*batch_size;
        if (update_count == do_count) marker = _edge_count;

        pgraph->create_marker(marker);
        pgraph->create_snapshot();

        cout << " Created snapshot at " << marker << endl;
        
        //update the sstream view
        if (eOK != sstreamh->update_view()) {
            usleep(100);
        }
        ++update_count;
    }
    cout << " RANK" << _rank 
         << " update_count = " << update_count << endl;
}