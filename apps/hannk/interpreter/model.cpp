#include "interpreter/model.h"
#include "util/error_util.h"

#include <cmath>
#include <list>

namespace hannk {

Tensor *apply(const TensorMap &map, const Tensor *t) {
    auto i = map.find(t);
    if (i != map.end()) {
        return i->second;
    }
    // TODO: Try to do this without const_cast?
    return const_cast<Tensor *>(t);
}

namespace {

HalideBuffer<void> make_buffer(halide_type_t type, const Box &bounds) {
    std::vector<int> extents(bounds.size());
    for (int i = 0; i < (int)bounds.size(); i++) {
        extents[i] = bounds[i].extent();
    }
    HalideBuffer<void> buffer(type, extents);
    for (int i = 0; i < (int)bounds.size(); i++) {
        buffer.translate(i, bounds[i].min);
    }
    return buffer;
}

}  // namespace


TensorStorage::TensorStorage() {}
TensorStorage::TensorStorage(HalideBuffer<void> buffer) : buffer_(buffer) {
    assert(!buffer_.data());
}

void TensorStorage::add_use(halide_type_t type, const Box &bounds) {
    if (buffer_.dimensions() == 0) {
        buffer_ = make_buffer(type, bounds);
    } else {
        assert(buffer_.type() == type);
        assert(buffer_.dimensions() == (int)bounds.size());
        assert(!buffer_.data());

        // Take the union of the existing buffer and the new bounds.
        for (int i = 0; i < rank(); i++) {
            int new_min = std::min(buffer_.dim(i).min(), bounds[i].min);
            int new_max = std::max(buffer_.dim(i).max(), bounds[i].max);
            buffer_.raw_buffer()->dim[i].min = new_min;
            buffer_.raw_buffer()->dim[i].extent = new_max - new_min + 1;
        }
    }
}

bool TensorStorage::is_allocated() const {
    return buffer_.data() != nullptr;
}

void TensorStorage::allocate() {
    if (!buffer_.data()) {
        buffer_ = HalideBuffer<void>::make_with_shape_of(buffer_);
    }
}


Tensor::Tensor(std::string name, HalideBuffer<void> buffer, QuantizationInfo quantization)
    : name_(std::move(name)),
      buffer_(std::move(buffer)),
      quantization_(std::move(quantization)) {
    is_constant_ = buffer_.data() != nullptr;
}

Tensor::Tensor(std::string name, halide_type_t type, const Box &bounds, QuantizationInfo quantization)
    : Tensor(name, make_buffer(type, bounds), quantization) {
}

std::shared_ptr<TensorStorage> Tensor::storage() {
    if (!storage_) {
        storage_ = std::make_shared<TensorStorage>(buffer_);
    }
    return storage_;
}

bool Tensor::is_allocated() const {
    return buffer_.data() != nullptr;
}

void Tensor::allocate() {
    if (buffer_.data()) {
        return;
    }

    storage()->allocate();
    HalideBuffer<void> buffer = storage()->buffer();
    for (int i = 0; i < buffer.dimensions(); i++) {
        assert(buffer.dim(i).min() <= buffer_.dim(i).min());
        assert(buffer.dim(i).max() >= buffer_.dim(i).max());
        buffer.crop(i, buffer_.dim(i).min(), buffer_.dim(i).extent());
    }
    buffer_ = buffer;
}

void Tensor::set_alias_of(Tensor *t) {
    storage_ = t->storage();
    storage_->add_use(type(), box());
}

void Tensor::dump(std::ostream &os) const {
    os << "  \"" << name() << "\" : "
       << "  " << buffer_.type() << " x ";

    const auto *b = buffer_.raw_buffer();
    os << '{';
    for (int i = 0; i < b->dimensions; i++) {
        if (i > 0) {
            os << ", ";
        }
        os << b->dim[i];
    }
    os << '}';

    if (is_allocated()) {
        os << " allocated " << name() << std::endl;
    } else {
        os << " " << name() << std::endl;
    }
}

Model::Model(const Model &copy) {
    // First, just copy all the tensors (shared pointers).
    tensors = copy.tensors;

    // Next, clone the non-allocated tensors. These might get intermediate state
    // while being executed.
    TensorMap map;
    for (auto &i : tensors) {
        if (!i->is_allocated()) {
            auto cloned = std::make_shared<Tensor>(*i);
            map[i.get()] = cloned.get();
            i = cloned;
        }
    }

    // Now copy the ops, using the tensor map we made above.
    for (const auto &i : copy.ops) {
        ops.push_back(i->clone(map));
    }
}

void Model::insert(std::shared_ptr<Tensor> to_insert, const Tensor *after) {
    for (auto i = tensors.begin(); i != tensors.end(); ++i) {
        if (i->get() == after) {
            tensors.insert(++i, to_insert);
            return;
        }
    }
    tensors.push_back(to_insert);
}

void Model::insert(std::unique_ptr<Op> to_insert, const Op *before) {
    for (auto i = ops.begin(); i != ops.end(); ++i) {
        if (i->get() == before) {
            ops.insert(i, std::move(to_insert));
            return;
        } else {
            for (int j = 0; j < to_insert->output_count(); ++j) {
                for (int k = 0; k < (*i)->input_count(); ++k) {
                    if ((*i)->input(k) == to_insert->output(j)) {
                        // i consumes an output of to_insert.
                        ops.insert(i, std::move(to_insert));
                        return;
                    }
                }
            }
        }
    }
    ops.push_back(std::move(to_insert));
}

void Model::accept(OpVisitor *v) {
    // TODO: Major hack, don't use iterators because visitors might invalidate them.
    for (int i = 0; i < (int)ops.size(); i++) {
        ops[i]->accept(v);
    }
}

void Model::dump(std::ostream &os) {
    os << "Tensors: " << std::endl;
    for (const auto &i : tensors) {
        i->dump(os);
    }

    os << "Ops: " << std::endl;
    for (const auto &i : ops) {
        i->dump(os);
    }
    os << std::endl;
}

}  // namespace hannk
