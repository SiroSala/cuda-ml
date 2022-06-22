#include "tensor.h"
#include "kernels.h"
#include "autodiff.h"

std::random_device device;
std::mt19937 random_number_generator{ device() };

Tensor::Tensor() = default;
Tensor::Tensor(const std::vector<int>& shape) :
    shape{ shape },
    rank{ shape.size() },
    strides( rank ),
    n_elements{ std::accumulate(shape.begin(), shape.end(), static_cast<size_t>(1), std::multiplies<size_t>()) },
    size{ n_elements * sizeof(float) }
{
    cudaMalloc(&data, size);
    size_t stride = 1;
    for (int i = rank - 1; i >= 0; --i) {
        strides[i] = stride;
        stride *= shape[i];
    }
}
float Tensor::operator[] (const std::vector<int>& indices) {
    float scalar;
    const size_t index{ std::inner_product(strides.begin(), strides.end(), indices.begin(), static_cast<size_t>(0)) };
    cudaMemcpy(&scalar, data + index, sizeof(float), cudaMemcpyDeviceToHost);
    return scalar;
}
Tensor Tensor::operator-() const {
    Tensor negation{ shape };
    negate<<<(n_elements + 255) / 256, 256>>>(n_elements, data, negation.data);
    if (backward) negation.backward = new NegateBackward{ backward };
    return negation;
}
Tensor Tensor::transpose(size_t dim1, size_t dim2) const {
    Tensor transpose{ *this };
    transpose.shape[dim1] = shape[dim2];
    transpose.shape[dim2] = shape[dim1];
    transpose.strides[dim1] = strides[dim2];
    transpose.strides[dim2] = strides[dim1];
    return transpose;
}
Tensor Tensor::gradients() const {
    return backward->tensors[0];
}
void Tensor::requires_gradients() {
    backward = new AccumulateGradients{};
}
Tensor operator+ (const Tensor& tensor1, const Tensor& tensor2) {
    size_t* tensor1_strides{};
    size_t* tensor2_strides{};
    size_t* strides{};
    Tensor sum{};
    prepare_broadcast(tensor1, tensor2, &tensor1_strides, &tensor2_strides, &strides, sum);
    add<<<(sum.n_elements + 255) / 256, 256>>>(sum.n_elements, sum.rank, tensor1_strides, tensor2_strides, strides, tensor1.data, tensor2.data, sum.data);
    if (tensor1.backward || tensor2.backward) sum.backward = new AddBackward{ {tensor1.backward, tensor2.backward} };
    return sum;
}
Tensor operator- (const Tensor& tensor1, const Tensor& tensor2) {
    size_t* tensor1_strides{};
    size_t* tensor2_strides{};
    size_t* strides{};
    Tensor difference{};
    prepare_broadcast(tensor1, tensor2, &tensor1_strides, &tensor2_strides, &strides, difference);
    subtract<<<(difference.n_elements + 255) / 256, 256>>>(difference.n_elements, difference.rank, tensor1_strides, tensor2_strides, strides, tensor1.data, tensor2.data, difference.data);
    if (tensor1.backward || tensor2.backward) difference.backward = new SubtractBackward{ {tensor1.backward, tensor2.backward} };
    return difference;

}
Tensor operator* (const Tensor& tensor1, const Tensor& tensor2) {
    size_t* tensor1_strides{};
    size_t* tensor2_strides{};
    size_t* strides{};
    Tensor product{};
    prepare_broadcast(tensor1, tensor2, &tensor1_strides, &tensor2_strides, &strides, product);
    multiply<<<(product.n_elements + 255) / 256, 256>>>(product.n_elements, product.rank, tensor1_strides, tensor2_strides, strides, tensor1.data, tensor2.data, product.data);
    if (tensor1.backward || tensor2.backward) product.backward = new MultiplyBackward{ {tensor1, tensor2}, {tensor1.backward, tensor2.backward} };
    return product;
}
Tensor operator/ (const Tensor& tensor1, const Tensor& tensor2) {
    size_t* tensor1_strides{};
    size_t* tensor2_strides{};
    size_t* strides{};
    Tensor quotient{};
    prepare_broadcast(tensor1, tensor2, &tensor1_strides, &tensor2_strides, &strides, quotient);
    divide<<<(quotient.n_elements + 255) / 256, 256>>>(quotient.n_elements, quotient.rank, tensor1_strides, tensor2_strides, strides, tensor1.data, tensor2.data, quotient.data);
    return quotient;
}
Tensor mm (const Tensor& tensor1, const Tensor& tensor2) {
    std::vector<int> shape{ tensor1.shape };
    shape.back() = tensor2.shape.back();
    Tensor matrix_product{ shape };
    const size_t height = matrix_product.shape.end()[-2];
    const size_t width = matrix_product.shape.end()[-1];
    const size_t shared_dim = tensor1.shape.end()[-1];
    const size_t batch_size = matrix_product.n_elements / (height * width);
    size_t* tensor1_strides{};
    size_t* tensor2_strides{};
    const size_t strides_size = matrix_product.rank * sizeof(size_t);
    cudaMalloc(&tensor1_strides, strides_size);
    cudaMalloc(&tensor2_strides, strides_size);
    cudaMemcpy(tensor1_strides, &tensor1.strides[0], strides_size, cudaMemcpyHostToDevice);
    cudaMemcpy(tensor2_strides, &tensor2.strides[0], strides_size, cudaMemcpyHostToDevice);
    dim3 block_dim(16, 16);
    dim3 grid_dim((height + block_dim.x - 1) / block_dim.x, (width + block_dim.y - 1) / block_dim.y, batch_size);
    matrix_multiply<<<grid_dim, block_dim>>>(matrix_product.rank, height, width, shared_dim, tensor1_strides, tensor2_strides, tensor1.data, tensor2.data, matrix_product.data);
    if (tensor1.backward || tensor2.backward) matrix_product.backward = new MatrixMultiplyBackward{ {tensor1, tensor2}, {tensor1.backward, tensor2.backward} };
    return matrix_product;
}
Tensor relu (const Tensor& input) {
    Tensor output{ input.shape };
    relu<<<(output.n_elements + 255) / 256, 256>>>(output.n_elements, input.data, output.data);
    if (input.backward) output.backward = new ReluBackward{ input, input.backward };
    return output;
}
Tensor relu_d (const Tensor& input) {
    Tensor output{ input.shape };
    relu_d<<<(output.n_elements + 255) / 256, 256>>>(output.n_elements, input.data, output.data);
    return output;
}
Tensor sum (const Tensor& input) {
    Tensor output{ std::vector<int>(input.rank, 1) };
    sum<<<1, 1>>>(input.n_elements, input.data, output.data);
    if (input.backward) output.backward = new SumBackward{ input.backward };
    return output;
}
std::ostream& operator<< (std::ostream& out, Tensor& tensor) {
    std::vector<int> indices(tensor.rank, 0);
    out << std::string(tensor.rank, '[');
    for (int i = 0; i < tensor.n_elements; ++i) {
        out << tensor[indices] << ", ";
        for (int j = tensor.rank - 1; j >= 0; --j) {
            if (indices[j] < (tensor.shape[j] - 1)) {
                out << std::string(tensor.rank - 1 - j, '[');
                ++indices[j];
                break;
            } else {
                out << "]";
                indices[j] = 0;
            }
        }
    }
    out << '\n';
    out << "shape = (";
    for (size_t dim: tensor.shape) out << dim << ", ";
    out << "), ";
    out << "rank = " << tensor.rank << ", ";
    out << "strides = (";
    for (size_t stride: tensor.strides) out << stride << ", ";
    out << "), ";
    out << "n_elements = " << tensor.n_elements << ", ";
    out << "size = " << tensor.size << ", ";
    out << '\n';
    return out;
}
Tensor Tensor::from_vector(const std::vector<float>& vector, const std::vector<int>& shape) {
    Tensor tensor{ shape };
    float* array = (float*)malloc(tensor.size);
    std::copy(vector.begin(), vector.end(), array);
    cudaMemcpy(tensor.data, array, tensor.size, cudaMemcpyHostToDevice);
    free(array);
    return tensor;
}
Tensor Tensor::from_scalar(float scalar, const std::vector<int>& shape) {
    Tensor tensor{ shape };
    float* array = (float*)malloc(tensor.size);
    std::fill_n(array, tensor.n_elements, scalar);
    cudaMemcpy(tensor.data, array, tensor.size, cudaMemcpyHostToDevice);
    free(array);
    return tensor;    
}
Tensor Tensor::random_uniform(float min, float max, const std::vector<int>& shape) {
    Tensor tensor{ shape };
    float* array = (float*)malloc(tensor.size);
    std::uniform_real_distribution<float> distribution{ min, max };
    for (int i = 0; i < tensor.n_elements; ++i) {
        array[i] = distribution(random_number_generator);
    }
    cudaMemcpy(tensor.data, array, tensor.size, cudaMemcpyHostToDevice);
    free(array);
    return tensor;    
}
Tensor Tensor::random_normal(float mean, float standard_deviation, const std::vector<int>& shape) {
    Tensor tensor{ shape };
    float* array = (float*)malloc(tensor.size);
    std::normal_distribution<float> distribution{ mean, standard_deviation };
    for (int i = 0; i < tensor.n_elements; ++i) {
        array[i] = distribution(random_number_generator);
    }
    cudaMemcpy(tensor.data, array, tensor.size, cudaMemcpyHostToDevice);
    free(array);
    return tensor;
}

void prepare_broadcast(const Tensor& tensor1, const Tensor& tensor2, size_t** d_tensor1_strides, size_t** d_tensor2_strides, size_t** d_strides, Tensor& sum){
    std::vector<int> shape{ tensor1.shape };
    size_t tensor1_strides[tensor1.rank]{ 0 };
    size_t tensor2_strides[tensor2.rank]{ 0 };
    for (int i = 0; i < tensor1.rank; ++i) {
        if (tensor1.shape[i] == tensor2.shape[i]) {
            tensor1_strides[i] = tensor1.strides[i];
            tensor2_strides[i] = tensor2.strides[i];
        }
        else if (tensor1.shape[i] > tensor2.shape[i]) {
            tensor1_strides[i] = tensor1.strides[i];
        }
        else {
            tensor2_strides[i] = tensor2.strides[i];
            shape[i] = tensor2.shape[i];
        }

    }
    sum = Tensor{ shape };
    const size_t strides_size = sum.rank * sizeof(size_t);
    cudaMalloc(d_strides, strides_size);
    cudaMalloc(d_tensor1_strides, strides_size);
    cudaMalloc(d_tensor2_strides, strides_size);
    cudaMemcpy(*d_strides, &sum.strides[0], strides_size, cudaMemcpyHostToDevice);
    cudaMemcpy(*d_tensor1_strides, tensor1_strides, strides_size, cudaMemcpyHostToDevice);
    cudaMemcpy(*d_tensor2_strides, tensor2_strides, strides_size, cudaMemcpyHostToDevice);
}
