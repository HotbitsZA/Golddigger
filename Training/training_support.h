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
- epsilon:  This parameter defines the epsilon-tube within which no penalty is associated 
            in the training loss function with points predicted within a distance epsilon 
            from the actual value. A smaller epsilon can lead to a more complex model that 
            fits the training data more closely, while a larger epsilon can lead to a simpler 
            model that may not fit the training data as closely but could generalize better 
            to unseen data.
            The error epsilon that determines when training should stop.
            Generally a good value for this is 0.001.  Smaller values may result
            in a more accurate solution but take longer to execute.
- gamma: This parameter defines how much influence a single training example has. The larger gamma is, the closer other examples must be to be affected. A smaller gamma means a wider influence of each support vector, while a larger gamma means a narrower influence. If gamma is too large, the model may overfit the training data, while if gamma is too small, the model may underfit.
- epsilon_insensitivity:    This is an internal parameter that controls the precision of the SVR solver. 
                            A smaller value can lead to a more accurate model but may increase 
                            training time, while a larger value can speed up training but may 
                            result in a less accurate model.
- cache_size:   This parameter specifies the cache size in megabytes for the SVR solver. 
                A larger cache size can speed up training on larger datasets but will use more 
                memory. The optimal cache size can depend on the size of the dataset and the 
                available system memory.
*/
struct TrainingHyperparameters
{
    double c{10.0};        // 10.0 is a common default for C in SVR, but this can be tuned based on the dataset.
    double epsilon{0.001}; // 0.001 is a common default for epsilon in SVR, but this can be tuned based on the dataset and the scale of the labels.
    double gamma{0.5};      // 0.1 is a common default for gamma in RBF kernels, but this can be tuned based on the dataset. A smaller gamma means a wider influence of each support vector, while a larger gamma means a narrower influence.
    // double epsilon_insensitivity{0.00001}; // Internal solver precision, smaller values can lead to a more accurate model but may increase training time. 
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
