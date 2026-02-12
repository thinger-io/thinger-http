#ifndef THINGER_DATA_BUFFER
#define THINGER_DATA_BUFFER

#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <cassert>
#include <cstring>

namespace thinger{

    class data_buffer{

    private:
        void * buffer_ = nullptr;
        size_t capacity_ = 0;
        size_t size_ = 0;
        size_t index_ = 0;

    public:
        data_buffer()= default;

        virtual ~data_buffer(){
            free(buffer_);
        }

        bool reserve(size_t size, size_t growing_step=1){
            if(growing_step != 1){
                size_t chunks = size / growing_step;
                if(size % growing_step != 0) chunks++;
                size = chunks * growing_step;
            }
            if(buffer_ == nullptr){
                buffer_ = malloc(size);
                if(buffer_ == nullptr) return false;
                capacity_ = size;
            }else{
                auto realloc_buffer = realloc(buffer_, size);
                if(realloc_buffer== nullptr) return false;
                buffer_ = realloc_buffer;
                capacity_ = size;
            }
            return true;
        }

        bool reserve_write_capacity(size_t size, size_t growing_step= 1){
            if(write_capacity() >= size) return true;
            return reserve(size_ + size, growing_step);
        }

        u_int8_t * data(){
            return static_cast<u_int8_t*>(buffer_);
        }

        u_int8_t * write_position(){
            return static_cast<u_int8_t*>(buffer_) + size_;
        }

        [[nodiscard]] size_t write_capacity() const{
            assert(capacity_ >= size_);
            return capacity_ - size_;
        }

        [[nodiscard]] size_t capacity() const{
            return capacity_;
        }

        [[nodiscard]] size_t size() const{
            return size_;
        }

        [[nodiscard]] size_t remaining() const{
            return size_ - index_;
        }

        void commit_write(size_t size){
            assert(capacity_ >= size_ + size);
            size_+=size;
        }

        [[nodiscard]] size_t index() const{
            return index_;
        }

        inline uint8_t operator[](size_t index){
            assert(index<capacity_ && index < size_);
            return static_cast<u_int8_t*>(buffer_)[index];
        }

        void commit_read(size_t size){
            assert(size_ >= size);
            if(size != 0 && size < size_){
                memmove(data(), data() + size, size_ - size);
            }
            size_ -= size;
            index_ = 0;
        }
    };
}

#endif