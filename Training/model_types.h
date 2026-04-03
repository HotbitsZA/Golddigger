#pragma once

#include <dlib/svm.h>

using sample_type = dlib::matrix<double, 15, 1>;
using kernel_type = dlib::radial_basis_kernel<sample_type>;
