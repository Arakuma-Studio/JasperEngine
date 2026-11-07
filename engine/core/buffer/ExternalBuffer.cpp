// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ExternalBuffer.h"

#include "Log.h"

using namespace jasperengine;

ExternalBuffer::ExternalBuffer(): Buffer(){}

ExternalBuffer::~ExternalBuffer(){

}

ExternalBuffer::ExternalBuffer(const ExternalBuffer& rhs): Buffer(rhs){
    name = rhs.name;
    ownedData = rhs.ownedData;
    modelBuffer = rhs.modelBuffer;
    modelOffset = rhs.modelOffset;
    if (rhs.ownsData()){
        data = ownedData.data();
    }
}

ExternalBuffer& ExternalBuffer::operator=(const ExternalBuffer& rhs){
    if (this == &rhs){
        return *this;
    }

    Buffer::operator =(rhs);

    name = rhs.name;
    ownedData = rhs.ownedData;
    modelBuffer = rhs.modelBuffer;
    modelOffset = rhs.modelOffset;
    if (rhs.ownsData()){
        data = ownedData.data();
    }

    return *this;
}

bool ExternalBuffer::increase(size_t newSize){
    clearShareSource();
    if (!ownsData()){
        try {
            std::vector<unsigned char>(newSize).swap(ownedData);
        }
        catch (const std::bad_alloc& e) {
            Log::error("Failed to increase buffer: out of memory");
            return false;
        }
        data = ownedData.empty() ? nullptr : ownedData.data();
        size = newSize;
        return true;
    }

    if (newSize >= ownedData.size()){
        try {
            ownedData.resize(newSize);
        }
        catch (const std::bad_alloc& e) {
            Log::error("Failed to increase buffer: out of memory");
            return false;
        }
        data = ownedData.data();
        return Buffer::increase(newSize);
    }

    return false;
}

void ExternalBuffer::clearAll(){
    Buffer::clearAll();

    std::vector<unsigned char>().swap(ownedData);
}

void ExternalBuffer::setData(unsigned char* data, size_t size){
    const bool intoOwned = !ownedData.empty() && data >= ownedData.data() && data < ownedData.data() + ownedData.size();
    if (!ownedData.empty() && !intoOwned){
        std::vector<unsigned char>().swap(ownedData);
    }

    this->data = data;
    this->size = size;
    modelBuffer = -1;
    dataReleased = false;
    clearShareSource();
}

void ExternalBuffer::setData(unsigned char* data, size_t size, const std::shared_ptr<const void>& owner){
    setData(data, size);
    // the bytes' address identifies them in the owner
    setShareSource(owner, (uint64_t)(uintptr_t)data);
}

void ExternalBuffer::setModelData(unsigned char* data, size_t size, const std::shared_ptr<const void>& owner, int buffer, size_t offset){
    setData(data, size);
    modelBuffer = buffer;
    modelOffset = offset;
    setShareSource(owner, ((uint64_t)(uint32_t)buffer << 40) | (uint64_t)offset);
}

int ExternalBuffer::getModelBuffer() const{
    return modelBuffer;
}

size_t ExternalBuffer::getModelOffset() const{
    return modelOffset;
}

void ExternalBuffer::releaseData(){
    data = nullptr;
    dataReleased = true;
}

void ExternalBuffer::restoreData(unsigned char* bytes){
    data = bytes;
    dataReleased = false;
}

void ExternalBuffer::copyToOwnData(){
    if (ownsData() || !data || size == 0){
        return;
    }

    std::vector<unsigned char> copy(data, data + size);
    ownedData.swap(copy);
    data = ownedData.data();
}

bool ExternalBuffer::ownsData() const{
    return !ownedData.empty() && data == ownedData.data();
}

void ExternalBuffer::setName(const std::string& name){
    this->name = name;
}

std::string ExternalBuffer::getName() const{
    return this->name;
}
