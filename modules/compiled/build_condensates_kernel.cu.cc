//#define GOOGLE_CUDA 1


#if GOOGLE_CUDA
#define EIGEN_USE_GPU

#include "build_condensates_kernel.h"
#include "helpers.h"
#include "tensorflow/core/util/gpu_kernel_helper.h"
#include <cuda.h>
#include <cuda_runtime.h>
#include <cuda_runtime_api.h>
#include "cuda_helpers.h"

namespace tensorflow {
namespace functor {

typedef Eigen::GpuDevice GPUDevice;


__global__
static void set_defaults(
        int *asso_idx,
        int * is_cpoint,
        const float * d_betas,
        float * temp_betas,
        const int n_vert){

    size_t i_v = blockIdx.x * blockDim.x + threadIdx.x;
    if(i_v>n_vert)
        return;

    asso_idx[i_v] = -1;

    temp_betas[i_v] = d_betas[i_v];

    is_cpoint[i_v] = 0;//not needed on GPU?
}


__device__
static float distancesq(const int v_a,
        const int v_b,
        const float *d_ccoords,
        const int n_ccoords){
    float distsq=0;
    for(size_t i=0;i<n_ccoords;i++){
        float xa = d_ccoords[I2D(v_a,i,n_ccoords)];
        float xb = d_ccoords[I2D(v_b,i,n_ccoords)];
        distsq += (xa-xb)*(xa-xb);
    }
    return distsq;
}

__global__
static void get_max_sub(

        const float* values,
        const int* ref_idxs,
        const int* mask,

        float * new_values,
        int * new_ref_idx,
        int * new_mask,

        const int n_values,

        const int n_subs,//includes a '+1'

        const float min_value){

    int tidx = blockIdx.x * blockDim.x + threadIdx.x;
    //printf("launched %d, nsub %d\n",tidx, n_subs);
    if(tidx>=n_subs)
        return;

    const int offset = tidx * n_values/n_subs;
    const int end = (tidx+1) * n_values/n_subs;//protect in loop

    // THIS NEEDS IMPROVEMENTS!

    //this needs to be some smart algo here
    //it will be called N_condensate times at least
    int local_ref=-1;
    float local_max = min_value;
    for(int i=offset;i<end;i++){

        if(i>=n_values)
            break;
        if(mask[i] >= 0)
            continue;
        float val_i = values[i];
        if(val_i > local_max){
            local_max=val_i;
            local_ref=i;
        }

    }

   // __syncthreads();

    int global_ref = local_ref;
    if(ref_idxs && global_ref>=0)
        global_ref = ref_idxs[local_ref];

   // printf("max in %d is %d\n",tidx,global_ref);

    //fill output
    new_ref_idx[tidx] = global_ref;
    if(local_ref>=0){
        new_values[tidx] = values[local_ref];
        new_mask[tidx] = mask[local_ref];
    }
    else{
        new_values[tidx] = min_value;
        new_mask[tidx] = -1;
    }
}




static float get_max_beta(
         float* temp_betas, //actually const

        const float* d_betas,
        int *asso_idx,
        int * is_cpoint,
        int * maxidx,

        const int n_vert,
        const int start_vertex,
        const int end_vertex,
        const float min_beta){



    const int n_total = end_vertex - start_vertex;

    float * tmp_values=temp_betas+start_vertex;
    int * tmp_ref_idx=NULL;
    int * tmp_mask=asso_idx+start_vertex;

    int sub_n_total = n_total;
    int n_sub = n_total+1;
    while(n_sub > 1){

        n_sub = sub_n_total/100 + 1; //loop 100 per thread
        if(n_sub < 10){
            n_sub = 1;
        }
        //printf("nsub: %d\n",n_sub);

        float * new_values=NULL;
        int * new_ref_idx=NULL;
        int * new_mask=NULL;

        if(cudaMalloc((void**)&new_values,n_sub*sizeof(float)) != cudaSuccess)
            printf("ERROR: get_max_beta mem alloc not successful.");
        if(cudaMalloc((void**)&new_ref_idx,n_sub*sizeof(int)) != cudaSuccess)
            printf("ERROR: get_max_beta mem alloc not successful.");
        if(cudaMalloc((void**)&new_mask,n_sub*sizeof(int)) != cudaSuccess)
            printf("ERROR: get_max_beta mem alloc not successful.");

        cudaDeviceSynchronize();

        grid_and_block gb(n_sub,256);
        //do
        //printf("launch kernel\n");
        get_max_sub<<<gb.grid(),gb.block()>>>(
                tmp_values,
                tmp_ref_idx,
                tmp_mask,
                new_values,
                new_ref_idx,
                new_mask,
                sub_n_total,
                n_sub,
                min_beta);

        cudaDeviceSynchronize();

        //if tmp delete tmp
        if(tmp_ref_idx){
            cudaFree(tmp_values);
            cudaFree(tmp_ref_idx);
            cudaFree(tmp_mask);
        }
        tmp_values = new_values;
        tmp_ref_idx = new_ref_idx;
        tmp_mask = new_mask;
        //set tmp to new
        sub_n_total = n_sub;

    }
    //printf("done, last nsub: %d\n",n_sub);
    //use tmp, delete tmp


    // collect output and clean up

    //copy final ref to CPU

    int ref=-1;
    cudaMemcpy(&ref, &tmp_ref_idx[0], sizeof(int), cudaMemcpyDeviceToHost);
    if(ref>=0)
        ref += start_vertex;
    *maxidx = ref;


    //printf("ref %d\n",ref);

    float ref_beta=0;
    if(ref>=0){
        int isc=1;
        cudaMemcpy(&is_cpoint[ref], &isc, sizeof(int), cudaMemcpyHostToDevice);
        cudaMemcpy(&asso_idx[ref], &ref, sizeof(int), cudaMemcpyHostToDevice);
        cudaMemcpy(&ref_beta, &d_betas[ref], sizeof(float), cudaMemcpyDeviceToHost);
    }


    cudaFree(tmp_values);
    cudaFree(tmp_ref_idx);
    cudaFree(tmp_mask);


    cudaDeviceSynchronize();
    return ref_beta;
}

__global__
static void check_and_collect(

        const int ref_vertex,
        const float ref_beta,
        const float *d_ccoords,
        const float *d_betas,

        int *asso_idx,
        float * temp_betas,

        const int n_vert,
        const int n_ccoords,

        const int start_vertex,
        const int end_vertex,
        const float radius,
        const float min_beta,
        const bool soft){

    int i_v = blockIdx.x * blockDim.x + threadIdx.x + start_vertex;
    if(i_v>=end_vertex)
        return;

    if(asso_idx[i_v] < 0){
        float distsq = distancesq(ref_vertex,i_v,d_ccoords,n_ccoords);

        if(soft){
            //should the reduction in beta be using the original betas or the modified ones...?
            //go with original betas
            float moddist = 1 - (distsq / radius );
            if(moddist < 0)
                moddist = 0;
            float subtract =  moddist * ref_beta;
            temp_betas[i_v] -= subtract;
            if(temp_betas[i_v] <= min_beta && moddist)
                asso_idx[i_v] = ref_vertex;
        }
        else{
            if(distsq <= radius){ //sum features in parallel?
                asso_idx[i_v] = ref_vertex;
            }
        }
    }


}





template <typename dummy>
struct BuildCondensatesOpFunctor<GPUDevice, dummy> {
    void operator()(
            const GPUDevice& d,

            const float *d_ccoords,
            const float *d_betas,
            const int *row_splits,

            int *asso_idx,
            int *is_cpoint,
            float * temp_betas,
            int *n_condensates,

            const int n_vert,
            const int n_ccoords,

            const int n_rs,

            const float radius,
            const float min_beta,
            const bool soft

) {

        grid_and_block gb_vert(n_vert, 512);

        set_defaults<<<gb_vert.grid(),gb_vert.block()>>>(asso_idx,is_cpoint,d_betas,temp_betas,n_vert);

        std::vector<int> cpu_rowsplits(n_rs);
        cudaMemcpy(&cpu_rowsplits.at(0),row_splits,n_rs*sizeof(int),cudaMemcpyDeviceToHost); //Async if needed, but these are just a few kB

        cudaDeviceSynchronize();
        //copy RS to CPU


        int ref=0;
        float ref_beta = 0;

        for(size_t j_rs=0;j_rs<n_rs-1;j_rs++){
            const int start_vertex = cpu_rowsplits[j_rs];
            const int end_vertex = cpu_rowsplits[j_rs+1];


            ref_beta = get_max_beta(temp_betas,d_betas,asso_idx,is_cpoint,&ref,n_vert,start_vertex,end_vertex,min_beta);
            //copy ref back

            //copy ref and refBeta from GPU to CPU

            grid_and_block gb_rsvert(end_vertex-start_vertex, 512);
            int ncond=0;
            while(ref>=0){

                // if(asso_idx[ref] >=0) continue; //
                // if(temp_betas[ref] < min_beta)continue;
                //probably better to copy here instead of accessing n_vert times in GPU mem
                ncond+=1;

                check_and_collect<<<gb_rsvert.grid(),gb_rsvert.block()>>>(
                        ref,
                        ref_beta,
                        d_ccoords,
                        d_betas,
                        asso_idx,
                        temp_betas,
                        n_vert,
                        n_ccoords,
                        start_vertex,
                        end_vertex,
                        radius,
                        min_beta,
                        soft);

                cudaDeviceSynchronize();

                ref_beta = get_max_beta(temp_betas,d_betas,asso_idx,is_cpoint,&ref,n_vert,start_vertex,end_vertex,min_beta);

            }

            cudaMemcpy(&n_condensates[j_rs], &ncond, sizeof(int), cudaMemcpyHostToDevice);


        }


    }


};



template struct BuildCondensatesOpFunctor<GPUDevice, int>;

}//functor
}//tensorflow


#endif  // GOOGLE_CUDA
