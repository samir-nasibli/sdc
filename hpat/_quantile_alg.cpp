#include "mpi.h"
#include <Python.h>
#include <random>
#include <cstdio>
#include <vector>
#include <cmath>
#include <algorithm>
#include <cassert>

#define root 0

std::pair<double, double> get_lower_upper_kth_parallel(std::vector<double> &my_array, int64_t total_size, int myrank, int n_pes, int64_t k);
double small_get_nth_parallel(std::vector<double> &my_array, int64_t total_size, int myrank, int n_pes, int64_t k);
double get_nth_parallel(std::vector<double> &my_array, int64_t k, int myrank, int n_pes);
double quantile_parallel(double* data, int64_t local_size, int64_t total_size, double quantile);

PyMODINIT_FUNC PyInit_quantile_alg(void) {
    PyObject *m;
    static struct PyModuleDef moduledef = {
            PyModuleDef_HEAD_INIT, "quantile_alg", "No docs", -1, NULL, };
    m = PyModule_Create(&moduledef);
    if (m == NULL)
        return NULL;

    PyObject_SetAttrString(m, "quantile_parallel",
                            PyLong_FromVoidPtr((void*)(&quantile_parallel)));
    return m;
}

double quantile_parallel(double* data, int64_t local_size, int64_t total_size, double quantile)
{
    int myrank, n_pes;
    MPI_Comm_size(MPI_COMM_WORLD, &n_pes);
    MPI_Comm_rank(MPI_COMM_WORLD, &myrank);
    std::vector<double> my_array(data, data+local_size);
    int64_t k = (int64_t)(quantile*total_size);
    double res = get_nth_parallel(my_array, k, myrank, n_pes);
    return res;
}

double get_nth_parallel(std::vector<double> &my_array, int64_t k, int myrank, int n_pes)
{
    int64_t local_size = my_array.size();
    int64_t total_size;
    MPI_Allreduce(&local_size, &total_size, 1, MPI_LONG_LONG_INT, MPI_SUM, MPI_COMM_WORLD);
    printf("total size: %ld k: %ld\n", total_size, k);
    int64_t threshold = (int64_t) pow(10.0, 7.0); // 100 million
    // int64_t threshold = 20;
    if (total_size < threshold)
    {
        return small_get_nth_parallel(my_array, total_size, myrank, n_pes, k);
    }
    else
    {
        std::pair<double, double> kths = get_lower_upper_kth_parallel(my_array, total_size, myrank, n_pes, k);
        double k1_val = kths.first;
        double k2_val = kths.second;
        printf("k1_val: %lf  k2_val: %lf\n", k1_val, k2_val);
        int64_t local_l0_num = 0, local_l1_num = 0, local_l2_num = 0;
        int64_t l0_num = 0, l1_num = 0, l2_num = 0;
        for(auto val: my_array)
        {
            if (val<k1_val)
                local_l0_num++;
            if (val>=k1_val && val<k2_val)
                local_l1_num++;
            if (val>=k2_val)
                local_l2_num++;
        }
        MPI_Allreduce(&local_l0_num, &l0_num, 1, MPI_LONG_LONG_INT, MPI_SUM, MPI_COMM_WORLD);
        MPI_Allreduce(&local_l1_num, &l1_num, 1, MPI_LONG_LONG_INT, MPI_SUM, MPI_COMM_WORLD);
        MPI_Allreduce(&local_l2_num, &l2_num, 1, MPI_LONG_LONG_INT, MPI_SUM, MPI_COMM_WORLD);
        // printf("set sizes: %ld %ld %ld\n", l0_num, l1_num, l2_num);
        assert(l0_num + l1_num + l2_num == total_size);
        // []----*---o----*-----]
        // if there are more elements in the last set than elemenet k to end,
        // this means k2 is equal to k
        if (l2_num > total_size-k)
            return k2_val;
        assert(l0_num < k);

        std::vector<double> new_my_array;
        int64_t new_k = k;

        int64_t new_ind = 0;
        if (k < l0_num)
        {
            printf("first set\n");
            new_my_array.resize(local_l0_num);
            // first set
            // throw away
            for(auto val: my_array)
            {
                if (val<k1_val)
                {
                    new_my_array[new_ind] = val;
                    new_ind++;
                }
            }
            // new_k doesn't change
        }
        else if (k < l0_num + l1_num)
        {
            printf("second set\n");
            // middle set
            new_my_array.resize(local_l1_num);
            for(auto val: my_array)
            {
                if (val>=k1_val && val<k2_val)
                {
                    new_my_array[new_ind] = val;
                    new_ind++;
                }
            }
            new_k -= l0_num;
        }
        else
        {
            printf("last set\n");
            // last set
            new_my_array.resize(local_l2_num);
            for(auto val: my_array)
            {
                if (val>=k2_val)
                {
                    new_my_array[new_ind] = val;
                    new_ind++;
                }
            }
            new_k -= (l0_num + l1_num);
        }
        return get_nth_parallel(new_my_array, new_k, myrank, n_pes);
    }
    return -1.0;
}

std::pair<double, double> get_lower_upper_kth_parallel(std::vector<double> &my_array, int64_t total_size, int myrank, int n_pes, int64_t k)
{
    int64_t local_size = my_array.size();
    std::default_random_engine r_engine(myrank);
    std::uniform_real_distribution<double> uniform_dist(0.0, 1.0);

    int64_t sample_size = (int64_t) (pow(10.0, 5.0)/n_pes); // 100000 total
    int64_t my_sample_size = std::min(sample_size, local_size);

    std::vector<double> my_sample;
    for(int64_t i=0; i<my_sample_size; i++)
    {
        int64_t index = (int64_t) (local_size*uniform_dist(r_engine));
        my_sample.push_back(my_array[index]);
    }
    /* select sample */
    // get total sample size;
    std::vector<double> all_sample_vec;
    int *rcounts = new int[n_pes];
    int *displs = new int[n_pes];
    int total_sample_size = 0;
    // gather the sample sizes
    MPI_Gather(&my_sample_size, 1, MPI_INT, rcounts, 1, MPI_INT, root, MPI_COMM_WORLD);
    // calculate size and displacements on root
    if (myrank == root)
    {
        for(int i=0; i<n_pes; i++)
        {
            // printf("rc %d\n", rcounts[i]);
            displs[i] = total_sample_size;
            total_sample_size += rcounts[i];
        }
        // printf("total sample size: %d\n", total_sample_size);
        all_sample_vec.resize(total_sample_size);
    }
    // gather sample data
    MPI_Gatherv(my_sample.data(), my_sample_size, MPI_DOUBLE, all_sample_vec.data(), rcounts, displs, MPI_DOUBLE, root, MPI_COMM_WORLD);
    double k1_val;
    double k2_val;
    if (myrank == root)
    {
        int local_k = (int) (k*(total_sample_size/(double)total_size));
        // printf("k:%ld local_k:%d\n", k, local_k);
        int k1 = (int) (local_k - sqrt(total_sample_size * log(total_size)));
        int k2 = (int) (local_k + sqrt(total_sample_size * log(total_size)));
        k1 = std::max(k1, 0);
        k2 = std::min(k2, total_sample_size-1);
        // printf("k1: %d k2: %d\n", k1, k2);
        std::nth_element(all_sample_vec.begin(), all_sample_vec.begin()+k1, all_sample_vec.end());
        k1_val = all_sample_vec[k1];
        std::nth_element(all_sample_vec.begin(), all_sample_vec.begin()+k2, all_sample_vec.end());
        k2_val = all_sample_vec[k2];
        printf("k1: %d k2: %d k1_val: %lf k2_val:%lf\n", k1, k2, k1_val, k2_val);
    }
    MPI_Bcast(&k1_val, 1, MPI_DOUBLE, root, MPI_COMM_WORLD);
    MPI_Bcast(&k2_val, 1, MPI_DOUBLE, root, MPI_COMM_WORLD);
    // cleanup
    delete[] rcounts;
    delete[] displs;
    return std::make_pair(k1_val, k2_val);
}

double small_get_nth_parallel(std::vector<double> &my_array, int64_t total_size, int myrank, int n_pes, int64_t k)
{
    double res;
    int my_data_size = my_array.size();
    int total_data_size = 0;
    std::vector<double> all_data_vec;

    // gather the data sizes
    int *rcounts = new int[n_pes];
    int *displs = new int[n_pes];
    MPI_Gather(&my_data_size, 1, MPI_INT, rcounts, 1, MPI_INT, root, MPI_COMM_WORLD);
    // calculate size and displacements on root
    if (myrank == root)
    {
        for(int i=0; i<n_pes; i++)
        {
            // printf("rc %d\n", rcounts[i]);
            displs[i] = total_data_size;
            total_data_size += rcounts[i];
        }
        printf("total small data size: %d\n", total_data_size);
        all_data_vec.resize(total_data_size);
    }
    // gather data
    MPI_Gatherv(my_array.data(), my_data_size, MPI_DOUBLE, all_data_vec.data(), rcounts, displs, MPI_DOUBLE, root, MPI_COMM_WORLD);
    // get nth element on root
    if (myrank == root)
    {
        std::nth_element(all_data_vec.begin(), all_data_vec.begin() + k, all_data_vec.end());
        res = all_data_vec[k];
    }
    MPI_Bcast(&res, 1, MPI_DOUBLE, root, MPI_COMM_WORLD);
    return res;
}
/*
    // double ep = log(sample_size)/ log(total_size);
    /for (size_t i = 0; i < local_size; i++) {
        if (uniform_dist(r_engine) <= select_probablity)
            my_sample.push_back(my_array[i]);
    }
    int64_t my_sample_size = my_sample.size();

/ double select_probablity =  pow(local_size, -ep); // N^-e
// MPI_Allreduce(&my_sample_size, &total_sample_size, 1, MPI_LONG_LONG_INT, MPI_SUM, MPI_COMM_WORLD);
*/
