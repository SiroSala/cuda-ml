#include <vector>
#include <numeric>
#include <functional>
#include <algorithm>
#include <random>
#include <iostream>

template <typename T> class Tensor; 
template <typename T> class Backward; 

std::random_device device;
std::mt19937 random_number_generator{ device() };

__device__
void get_indices(size_t index, size_t rank, size_t* strides1, size_t* strides2, size_t* strides, size_t* indices)
{
    size_t index_remainder = index;
    indices[0] = 0;
    indices[1] = 0;
    for (int i = 0; i < rank; ++i) {
        const size_t dim = index_remainder / strides[i];
        index_remainder -= dim * strides[i];
        indices[0] += dim * strides1[i];
        indices[1] += dim * strides2[i];
    }
}

template <typename T>
__global__
void add(size_t n, size_t rank, size_t* tensor1_strides, size_t* tensor2_strides, size_t* strides, T* tensor1, T* tensor2, T* sum)
{
  const size_t index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index < n) {
      size_t indices[2];
      get_indices(index, rank, tensor1_strides, tensor2_strides, strides, indices);
      sum[index] = tensor1[indices[0]] + tensor2[indices[1]];
  }
}

template <typename T>
__global__
void subtract(size_t n, size_t rank, size_t* tensor1_strides, size_t* tensor2_strides, size_t* strides, T* tensor1, T* tensor2, T* difference)
{
  const int index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index < n) {
      size_t indices[2];
      get_indices(index, rank, tensor1_strides, tensor2_strides, strides, indices);
      difference[index] = tensor1[indices[0]] - tensor2[indices[1]];
  }
}

template <typename T>
__global__
void multiply(size_t n, size_t rank, size_t* tensor1_strides, size_t* tensor2_strides, size_t* strides, T* tensor1, T* tensor2, T* product)
{
  const size_t index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index < n) {
      size_t indices[2];
      get_indices(index, rank, tensor1_strides, tensor2_strides, strides, indices);
      product[index] = tensor1[indices[0]] * tensor2[indices[1]];
  }
}

template <typename T>
__global__
void divide(size_t n, size_t rank, size_t* tensor1_strides, size_t* tensor2_strides, size_t* strides, T* tensor1, T* tensor2, T* quotient)
{
  const size_t index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index < n) {
      size_t indices[2];
      get_indices(index, rank, tensor1_strides, tensor2_strides, strides, indices);
      quotient[index] = tensor1[indices[0]] / tensor2[indices[1]];
  }
}

template <typename T>
__global__
void matrix_multiply(size_t rank, size_t height, size_t width, size_t shared_dim, size_t* tensor1_strides, size_t* tensor2_strides, T* tensor1, T* tensor2, T* matrix_product)
{
    const size_t row = blockIdx.x * blockDim.x + threadIdx.x;
    const size_t column = blockIdx.y * blockDim.y + threadIdx.y;
    if (row < height && column < width) {
        const size_t tensor1_start = blockIdx.z * height * shared_dim + row * tensor1_strides[rank - 2];
        const size_t tensor2_start = blockIdx.z * width * shared_dim + column * tensor2_strides[rank - 1];
        T product{ 0 };
        for (int i = 0; i < shared_dim; ++i) {
            product += tensor1[tensor1_start + i * tensor1_strides[rank - 1]] * tensor2[tensor2_start + i * tensor2_strides[rank - 2]];
        }
        matrix_product[blockIdx.z * height * width + row * width + column] = product;
    }
}

template <typename T>
__global__
void negate(size_t n, T* input, T* output)
{
  const int i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i < n) output[i] = -input[i];
}

template <typename T>
__global__
void sum(size_t n, T* input, T* output)
{
  T sum{ 0 };
  for (int i = 0; i < n; ++i) {
      sum += input[i];
  }
  output[0] = sum;
}

template <typename T>
__global__
void relu(size_t n, T* input, T* output)
{
  const int i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i < n) output[i] = input[i] > 0 ? input[i] : 0;
}

template <typename T>
__global__
void relu_d(size_t n, T* input, T* output)
{
  const int i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i < n) output[i] = input[i] > 0 ? 1 : 0;
}

template <typename T>
class Backward
{
public:
    std::vector<Tensor<T>> tensors{};
    const std::vector<Backward<T>*> backwards{};
    Backward() = default;
    Backward(const std::vector<Backward<T>*>& backwards) : backwards{ backwards } {}
    Backward(const std::vector<Tensor<T>>& tensors, const std::vector<Backward<T>*>& backwards) : tensors{ tensors }, backwards{ backwards } {}
    virtual void operator() (const Tensor<T>& gradients) {}
};

template <typename T>
class AccumulateGradients : public Backward<T>
{
public:
    virtual void operator() (const Tensor<T>& gradients) {
        this->tensors.push_back(gradients);
    }
};

template <typename T>
class MultiplyBackward : public Backward<T>
{
public:
    MultiplyBackward(const std::vector<Tensor<T>>& tensors, const std::vector<Backward<T>*>& backwards) : Backward<T>{ tensors, backwards } {}
    virtual void operator() (const Tensor<T>& gradients) {
        if (this->backwards[0]) (*this->backwards[0])(gradients * this->tensors[1]);
        if (this->backwards[1]) (*this->backwards[1])(gradients * this->tensors[0]);
    }
};

template <typename T>
void prepare_broadcast(const Tensor<T>& tensor1, const Tensor<T>& tensor2, size_t** d_tensor1_strides, size_t** d_tensor2_strides, size_t** d_strides, Tensor<T>& sum){
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
    sum = Tensor<T>{ shape };
    const size_t strides_size = sum.rank * sizeof(size_t);
    cudaMalloc(d_strides, strides_size);
    cudaMalloc(d_tensor1_strides, strides_size);
    cudaMalloc(d_tensor2_strides, strides_size);
    cudaMemcpy(*d_strides, &sum.strides[0], strides_size, cudaMemcpyHostToDevice);
    cudaMemcpy(*d_tensor1_strides, tensor1_strides, strides_size, cudaMemcpyHostToDevice);
    cudaMemcpy(*d_tensor2_strides, tensor2_strides, strides_size, cudaMemcpyHostToDevice);
}

template <typename T>
class Tensor {
private:
    T* data{};
public:
    std::vector<int> shape{};
    size_t rank{};
    std::vector<size_t> strides{};
    size_t n_elements{};
    size_t size{};
    Backward<T>* backward{};
    Tensor() = default;
    Tensor(const std::vector<int>& shape) :
        shape{ shape },
        rank{ shape.size() },
        strides( rank ),
        n_elements{ std::accumulate(shape.begin(), shape.end(), static_cast<size_t>(1), std::multiplies<size_t>()) },
        size{ n_elements * sizeof(T) }
    {
        cudaMalloc(&data, size);
        size_t stride = 1;
        for (int i = rank - 1; i >= 0; --i) {
            strides[i] = stride;
            stride *= shape[i];
        }
    }
    T operator[] (const std::vector<int>& indices) {
        T scalar;
        const size_t index{ std::inner_product(strides.begin(), strides.end(), indices.begin(), static_cast<size_t>(0)) };
        cudaMemcpy(&scalar, data + index, sizeof(T), cudaMemcpyDeviceToHost);
        return scalar;
    }
    Tensor<T> operator-() const {
        Tensor<T> negation{ shape };
        negate<T><<<(n_elements + 255) / 256, 256>>>(n_elements, data, negation.data);
        return negation;
    }
    Tensor<T> transpose(size_t dim1, size_t dim2) const {
        Tensor<T> transpose{ *this };
        transpose.shape[dim1] = shape[dim2];
        transpose.shape[dim2] = shape[dim1];
        transpose.strides[dim1] = strides[dim2];
        transpose.strides[dim2] = strides[dim1];
        return transpose;
    }
    Tensor<T> gradients() const {
        return backward->tensors[0];
    }
    void requires_gradients() {
        backward = new AccumulateGradients<T>{};
    }
    friend Tensor<T> operator+ (const Tensor<T>& tensor1, const Tensor<T>& tensor2) {
        size_t* tensor1_strides{};
        size_t* tensor2_strides{};
        size_t* strides{};
        Tensor<T> sum{};
        prepare_broadcast<T>(tensor1, tensor2, &tensor1_strides, &tensor2_strides, &strides, sum);
        add<T><<<(sum.n_elements + 255) / 256, 256>>>(sum.n_elements, sum.rank, tensor1_strides, tensor2_strides, strides, tensor1.data, tensor2.data, sum.data);
        return sum;
    }
    friend Tensor<T> operator- (const Tensor<T>& tensor1, const Tensor<T>& tensor2) {
        size_t* tensor1_strides{};
        size_t* tensor2_strides{};
        size_t* strides{};
        Tensor<T> difference{};
        prepare_broadcast<T>(tensor1, tensor2, &tensor1_strides, &tensor2_strides, &strides, difference);
        subtract<T><<<(difference.n_elements + 255) / 256, 256>>>(difference.n_elements, difference.rank, tensor1_strides, tensor2_strides, strides, tensor1.data, tensor2.data, difference.data);
        return difference;
    }
    friend Tensor<T> operator* (const Tensor<T>& tensor1, const Tensor<T>& tensor2) {
        size_t* tensor1_strides{};
        size_t* tensor2_strides{};
        size_t* strides{};
        Tensor<T> product{};
        prepare_broadcast<T>(tensor1, tensor2, &tensor1_strides, &tensor2_strides, &strides, product);
        multiply<T><<<(product.n_elements + 255) / 256, 256>>>(product.n_elements, product.rank, tensor1_strides, tensor2_strides, strides, tensor1.data, tensor2.data, product.data);
        product.backward = new MultiplyBackward<T>{ {tensor1, tensor2}, {tensor1.backward, tensor2.backward} };
        return product;
    }
    friend Tensor<T> operator/ (const Tensor<T>& tensor1, const Tensor<T>& tensor2) {
        size_t* tensor1_strides{};
        size_t* tensor2_strides{};
        size_t* strides{};
        Tensor<T> quotient{};
        prepare_broadcast<T>(tensor1, tensor2, &tensor1_strides, &tensor2_strides, &strides, quotient);
        divide<T><<<(quotient.n_elements + 255) / 256, 256>>>(quotient.n_elements, quotient.rank, tensor1_strides, tensor2_strides, strides, tensor1.data, tensor2.data, quotient.data);
        return quotient;
    }
    friend Tensor<T> mm (const Tensor<T>& tensor1, const Tensor<T>& tensor2) {
        std::vector<int> shape{ tensor1.shape };
        shape.back() = tensor2.shape.back();
        Tensor<T> matrix_product{ shape };
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
        matrix_multiply<T><<<grid_dim, block_dim>>>(matrix_product.rank, height, width, shared_dim, tensor1_strides, tensor2_strides, tensor1.data, tensor2.data, matrix_product.data);
        return matrix_product;
    }
    friend Tensor<T> relu (const Tensor<T>& input) {
        Tensor<T> output{ input.shape };
        relu<T><<<(output.n_elements + 255) / 256, 256>>>(output.n_elements, input.data, output.data);
        return output;
    }
    friend Tensor<T> relu_d (const Tensor<T>& input) {
        Tensor<T> output{ input.shape };
        relu_d<T><<<(output.n_elements + 255) / 256, 256>>>(output.n_elements, input.data, output.data);
        return output;
    }
    friend Tensor<T> sum (const Tensor<T>& input) {
        Tensor<T> sum{ std::vector<int>(input.rank, 1) };
        relu_d<T><<<1, 1>>>(input.n_elements, input.data, sum.data);
        return sum;
    }
    friend std::ostream& operator<< (std::ostream& out, Tensor<T>& tensor) {
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
    static Tensor<T> from_vector(const std::vector<T>& vector, const std::vector<int>& shape) {
        Tensor<T> tensor{ shape };
        T* array = (T*)malloc(tensor.size);
        std::copy(vector.begin(), vector.end(), array);
        cudaMemcpy(tensor.data, array, tensor.size, cudaMemcpyHostToDevice);
        free(array);
        return tensor;
    }
    static Tensor<T> from_scalar(T scalar, const std::vector<int>& shape) {
        Tensor<T> tensor{ shape };
        T* array = (T*)malloc(tensor.size);
        std::fill_n(array, tensor.n_elements, scalar);
        cudaMemcpy(tensor.data, array, tensor.size, cudaMemcpyHostToDevice);
        free(array);
        return tensor;    
    }
    static Tensor<T> random_uniform(T min, T max, const std::vector<int>& shape) {
        Tensor<T> tensor{ shape };
        T* array = (T*)malloc(tensor.size);
        std::uniform_real_distribution<T> distribution{ min, max };
        for (int i = 0; i < tensor.n_elements; ++i) {
            array[i] = distribution(random_number_generator);
        }
        cudaMemcpy(tensor.data, array, tensor.size, cudaMemcpyHostToDevice);
        free(array);
        return tensor;    
    }
    static Tensor<T> random_normal(T mean, T standard_deviation, const std::vector<int>& shape) {
        Tensor<T> tensor{ shape };
        T* array = (T*)malloc(tensor.size);
        std::normal_distribution<T> distribution{ mean, standard_deviation };
        for (int i = 0; i < tensor.n_elements; ++i) {
            array[i] = distribution(random_number_generator);
        }
        cudaMemcpy(tensor.data, array, tensor.size, cudaMemcpyHostToDevice);
        free(array);
        return tensor;
    }
};
