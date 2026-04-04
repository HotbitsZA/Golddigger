#pragma once

#include "Training/model_types.h"

#include <cstddef>
#include <string>
#include <vector>

struct TrainingDataset
{
    std::vector<sample_type> samples;
    std::vector<double> labels;
    std::size_t candleCount{0};
};

// Notes on Training Hyperparameters:
/*
- C:    This parameter controls the trade-off between achieving a low training error and a low testing error,
        which is equivalent to controlling the trade-off between a smooth decision boundary and
        classifying the training points correctly. A smaller C encourages a smoother decision 
        boundary, while a larger C aims to classify all training examples correctly, 
        which can lead to overfitting if C is too large.
        The SVR regularization parameter.  It is the parameter that 
        determines the trade-off between trying to reduce the training error 
        or allowing more errors but hopefully improving the generalization 
        of the resulting decision_function.  Larger values encourage exact 
        fitting while smaller values of C may encourage better generalization.
- epsilon:  This is the epsilon-insensitive regression tube size used by
            `svr_trainer::set_epsilon_insensitivity()`. Predictions that land
            within this tube of the target incur no loss. Smaller values try to
            fit the training data more tightly, while larger values usually
            produce smoother models.
- gamma: This parameter defines how much influence a single training example has. The larger gamma is, the closer other examples must be to be affected. A smaller gamma means a wider influence of each support vector, while a larger gamma means a narrower influence. If gamma is too large, the model may overfit the training data, while if gamma is too small, the model may underfit.
- solver_epsilon:   The internal SVR solver stopping tolerance is kept fixed in
                    code at a sane default instead of being tuned. Smaller
                    values can increase runtime substantially.
- cache_size:   This parameter specifies the cache size in megabytes for the SVR solver. 
                A larger cache size can speed up training on larger datasets but will use more 
                memory. The optimal cache size can depend on the size of the dataset and the 
                available system memory.
*/
struct TrainingHyperparameters
{
    double c{10.0};        // 10.0 is a common default for C in SVR, but this can be tuned based on the dataset.
    double epsilon{0.001}; // Stored as epsilon-insensitivity for the SVR regression tube.
    double gamma{0.5};     // Default gamma for the RBF kernel; tune based on the dataset.
    // double cache_size{2000}; // Cache size in MB for the SVR solver, larger values can speed up training on larger datasets but will use more memory.
};

inline void serialize(const TrainingHyperparameters &parameters, std::ostream &out)
{
    dlib::serialize(parameters.c, out);
    dlib::serialize(parameters.epsilon, out);
    dlib::serialize(parameters.gamma, out);
}

inline void deserialize(TrainingHyperparameters &parameters, std::istream &in)
{
    dlib::deserialize(parameters.c, in);
    dlib::deserialize(parameters.epsilon, in);
    dlib::deserialize(parameters.gamma, in);
}

TrainingHyperparameters default_training_hyperparameters();
TrainingDataset load_training_dataset(const std::string &dataFile);
std::string build_default_tuner_path(const std::string &dataFile);
TrainingHyperparameters load_tuner_parameters(const std::string &path);
TrainingHyperparameters load_tuner_parameters_for_data_file(const std::string &dataFile);
void save_tuner_parameters(const std::string &path, const TrainingHyperparameters &parameters);
