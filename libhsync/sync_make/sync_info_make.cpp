//  sync_info_make.cpp
//  sync_make
//  Created by housisong on 2019-09-17.
/*
 The MIT License (MIT)
 Copyright (c) 2019-2022 HouSisong
 
 Permission is hereby granted, free of charge, to any person
 obtaining a copy of this software and associated documentation
 files (the "Software"), to deal in the Software without
 restriction, including without limitation the rights to use,
 copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the
 Software is furnished to do so, subject to the following
 conditions:
 
 The above copyright notice and this permission notice shall be
 included in all copies of the Software.
 
 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 OTHER DEALINGS IN THE SOFTWARE.
 */
#include "sync_info_make.h"
#include "sync_make_hash_clash.h" // isCanUse32bitRollHash
#include "../sync_client/sync_info_client.h" // TNewDataSyncInfo_dir_saveHeadTo
using namespace hdiff_private;
namespace sync_private{

#define _outBuf(_buf_begin,_buf_end) { \
            checksumInfo.append(_buf_begin,_buf_end); \
            writeStream(out_stream,outPos,_buf_begin,_buf_end); }
#define _flushV(_v)  if (!_v.empty()){ _outBuf(_v.data(),_v.data()+_v.size()); _v.clear(); }
#define _flushV_end(_v)  if (!_v.empty()){ _flushV(_v); swapClear(_v); }

    static void saveSamePairList(std::vector<TByte> &buf,
                                 const TSameNewBlockPair* samePairList, size_t samePairCount) {
        uint32_t pre=0;
        for (size_t i=0;i<samePairCount;++i){
            const TSameNewBlockPair& sp=samePairList[i];
            packUInt(buf,(uint32_t)(sp.curIndex-pre));
            packUInt(buf,(uint32_t)(sp.curIndex-sp.sameIndex));
            pre=sp.curIndex;
        }
    }
    
    static void saveSavedSizes(std::vector<TByte> &buf, TNewDataSyncInfo *self) {
        const uint32_t kBlockCount=(uint32_t)TNewDataSyncInfo_blockCount(self);
        for (uint32_t i=0; i<kBlockCount; ++i){
            uint32_t savedSize=self->savedSizes[i];
            if (savedSize==TNewDataSyncInfo_newDataBlockSize(self,i))
                savedSize=0;
            packUInt(buf,savedSize);
        }
    }
    
    static void _compressBuf(std::vector<TByte> &buf, const hsync_TDictCompress *compressPlugin) {
        std::vector<TByte> cmbuf;
        cmbuf.resize((size_t)compressPlugin->maxCompressedSize(buf.size()));
        hsync_dictCompressHandle  compressHandle=compressPlugin->dictCompressOpen(compressPlugin);
        checkv(compressHandle!=0);
        size_t compressedSize=compressPlugin->dictCompress(compressHandle,cmbuf.data(),cmbuf.data()+cmbuf.size(),
                                                           buf.data(),buf.data(),buf.data()+buf.size(),
                                                           hpatch_TRUE,hpatch_TRUE);
        compressPlugin->dictCompressClose(compressPlugin,compressHandle);
        if ((compressedSize>0)&&(compressedSize<buf.size())){
            cmbuf.resize(compressedSize);
            buf.swap(cmbuf);
        }
    }
    
    static void savePartHash(const hpatch_TStreamOutput *out_stream,hpatch_StreamPos_t &outPos,
                             uint32_t kBlockCount,const TByte* partHash,size_t partSize,
                             const TSameNewBlockPair* samePairList,uint32_t samePairCount,CChecksum &checksumInfo) {
        std::vector<TByte> buf;
        uint32_t curPair=0;
        for (size_t i=0; i<kBlockCount; ++i,partHash+=partSize){
            if ((curPair<samePairCount)&&(i==samePairList[curPair].curIndex))
                { ++curPair; continue; }
            pushBack(buf,partHash,partSize);
            if (buf.size()>=hpatch_kFileIOBufBetterSize)
                _flushV(buf);
        }
        _flushV(buf);
        assert(curPair==samePairCount);
    }
    
#if (_IS_NEED_DIR_DIFF_PATCH)
static void _saveDirHeadTo(const TNewDataSyncInfo_dir* self,std::vector<hpatch_byte>& out_buf){
    packUInt(out_buf,self->dir_newExecuteCount);
    packUInt(out_buf,self->dir_newPathCount);
    packUInt(out_buf,self->dir_newPathSumCharSize);
}

void TNewDataSyncInfo_dirWithHead_saveTo(TNewDataSyncInfo_dir* self,std::vector<hpatch_byte>& out_buf){
    size_t pos=out_buf.size();
    TNewDataSyncInfo_dir_saveTo(self,out_buf);

    std::vector<hpatch_byte> head;
    _saveDirHeadTo(self,head);
    out_buf.insert(out_buf.begin()+pos,head.begin(),head.end());
}
#endif

void TNewDataSyncInfo_saveTo(TNewDataSyncInfo* self,const hpatch_TStreamOutput* out_stream,
                             const hsync_TDictCompress* compressPlugin,hsync_dictCompressHandle dictHandle,size_t dictSize){
#if (_IS_NEED_DIR_DIFF_PATCH)
    if (self->isDirSyncInfo)
        checkv(self->dirInfoSavedSize>0);
#else
    checkv(!self->isDirSyncInfo);
#endif
    const char* kVersionType=self->isDirSyncInfo?"HDirSyni22":"HSyni22";
    if (compressPlugin)
        checkv(0==strcmp(compressPlugin->compressType(),self->compressType));
    else
        checkv(self->compressType==0);
    hpatch_TChecksum* strongChecksumPlugin=self->_strongChecksumPlugin;
    checkv(0==strcmp(strongChecksumPlugin->checksumType(),self->strongChecksumType));

    const size_t privateExternDataSize=0; //reserved ,now empty
    const size_t externDataSize=0;//reserved ,now empty
    const uint8_t isSavedSizes=(self->savedSizes)!=0?1:0;
    std::vector<TByte> buf;
    
    saveSamePairList(buf,self->samePairList,self->samePairCount);
    if (isSavedSizes)
        saveSavedSizes(buf,self);

    //compress buf
    size_t uncompressDataSize=buf.size();
    size_t compressDataSize=0;
    if (compressPlugin){
        _compressBuf(buf,compressPlugin);
        if (buf.size()<uncompressDataSize)
            compressDataSize=buf.size();
    }
    
    std::vector<TByte> head;
    {//head
        pushTypes(head,kVersionType,compressPlugin?compressPlugin->compressType():0,strongChecksumPlugin);
        packUInt(head,dictSize);
        packUInt(head,self->newSyncDataSize);
        packUInt(head,self->newSyncDataOffsert);
        packUInt(head,self->newDataSize);
        packUInt(head,self->kSyncBlockSize);
        packUInt(head,self->kStrongChecksumByteSize);
        packUInt(head,self->savedStrongChecksumByteSize);
        packUInt(head,self->savedRollHashByteSize);
        packUInt(head,self->samePairCount);
        pushUInt(head,isSavedSizes);
        packUInt(head,privateExternDataSize);
        packUInt(head,externDataSize);
        packUInt(head,uncompressDataSize);
        packUInt(head,compressDataSize);
        pushUInt(head,self->isDirSyncInfo);
#if (_IS_NEED_DIR_DIFF_PATCH)
        if (self->isDirSyncInfo){
            packUInt(head,self->dirInfoSavedSize);
            _saveDirHeadTo(&self->dirInfo,head);
        }
#endif
        
        {//newSyncInfoSize
            self->newSyncInfoSize = head.size()+privateExternDataSize+externDataSize+buf.size();
            self->newSyncInfoSize +=(self->savedRollHashByteSize+self->savedStrongChecksumByteSize)
                                    *(TNewDataSyncInfo_blockCount(self)-self->samePairCount);
            self->newSyncInfoSize += self->kStrongChecksumByteSize+self->kStrongChecksumByteSize;
            {
                hpatch_uint pksize=hpatch_packUInt_size(self->newSyncInfoSize);
                self->newSyncInfoSize+=pksize;
                if (hpatch_packUInt_size(self->newSyncInfoSize)>pksize){
                    assert(hpatch_packUInt_size(self->newSyncInfoSize)==pksize+1);
                    self->newSyncInfoSize++;
                    assert(hpatch_packUInt_size(self->newSyncInfoSize)==pksize+1);
                }
            }
            packUInt(head,self->newSyncInfoSize);
        }
        //end head info
    }
    hpatch_StreamPos_t kBlockCount=TNewDataSyncInfo_blockCount(self);
    checkv(kBlockCount==(uint32_t)kBlockCount);

    CChecksum checksumInfo(strongChecksumPlugin);
    hpatch_StreamPos_t outPos=0;
    //out head buf
    _flushV_end(head);
    //adding newDataCheckChecksum
    _outBuf(self->savedNewDataCheckChecksum,self->savedNewDataCheckChecksum+self->kStrongChecksumByteSize);
    assert(privateExternDataSize==0);//out privateExternData //reserved ,now empty
    assert(externDataSize==0);//reserved ,now empty
    _flushV_end(buf);
    
    savePartHash(out_stream,outPos,(uint32_t)kBlockCount,self->rollHashs,self->savedRollHashByteSize,
                 self->samePairList,self->samePairCount,checksumInfo);
    savePartHash(out_stream,outPos,(uint32_t)kBlockCount,self->partChecksums,self->savedStrongChecksumByteSize,
                 self->samePairList,self->samePairCount,checksumInfo);
    
    {// out infoFullChecksum
        checksumInfo.appendEnd();
        assert(checksumInfo.checksum.size()==self->kStrongChecksumByteSize);
        memcpy(self->infoFullChecksum,checksumInfo.checksum.data(),checksumInfo.checksum.size());
        writeStream(out_stream,outPos,self->infoFullChecksum,checksumInfo.checksum.size());
        assert(outPos==self->newSyncInfoSize);
    }
}


CNewDataSyncInfo::CNewDataSyncInfo(hpatch_TChecksum* strongChecksumPlugin,const hsync_TDictCompress* compressPlugin,
                                   hpatch_StreamPos_t newDataSize,uint32_t kSyncBlockSize,size_t kSafeHashClashBit){
    TNewDataSyncInfo_init(this);
    if (compressPlugin){
        this->_compressType.assign(compressPlugin->compressType());
        this->compressType=this->_compressType.c_str();
    }
    this->_strongChecksumPlugin=strongChecksumPlugin;
    this->_strongChecksumType.assign(strongChecksumPlugin->checksumType());
    this->strongChecksumType=this->_strongChecksumType.c_str();
    this->kStrongChecksumByteSize=(uint32_t)strongChecksumPlugin->checksumByteSize();
    this->kSyncBlockSize=kSyncBlockSize;
    this->newDataSize=newDataSize;
    getNeedHashByte(kSafeHashClashBit,newDataSize,kSyncBlockSize,this->kStrongChecksumByteSize,
                    &this->savedRollHashByteSize,&this->savedStrongChecksumByteSize);
    //mem
    const size_t kBlockCount=this->blockCount();
    hpatch_StreamPos_t memSize=kBlockCount*( (hpatch_StreamPos_t)0
                                            +sizeof(TSameNewBlockPair)+this->savedStrongChecksumByteSize
                                            +this->savedRollHashByteSize +(compressPlugin?sizeof(uint32_t):0));
    memSize+=this->kStrongChecksumByteSize+checkChecksumBufByteSize(this->kStrongChecksumByteSize);
    checkv(memSize==(size_t)memSize);
    _mem.realloc((size_t)memSize);
    TByte* curMem=_mem.data();
    
    this->infoFullChecksum=curMem; curMem+=this->kStrongChecksumByteSize;
    this->savedNewDataCheckChecksum=curMem; curMem+=checkChecksumBufByteSize(this->kStrongChecksumByteSize);
    this->partChecksums=curMem; curMem+=kBlockCount*this->savedStrongChecksumByteSize;
    this->samePairCount=0;
    this->samePairList=(TSameNewBlockPair*)curMem; curMem+=kBlockCount*sizeof(TSameNewBlockPair);
    this->rollHashs=curMem; curMem+=kBlockCount*this->savedRollHashByteSize;
    if (compressPlugin){
        this->savedSizes=(uint32_t*)curMem; curMem+=kBlockCount*sizeof(uint32_t);
    }
    assert(curMem==_mem.data_end());
}


static hpatch_StreamPos_t _hsynz_write_head(struct hsync_THsynz* zPlugin,
                                            const hpatch_TStreamOutput* out_stream,hpatch_StreamPos_t curOutPos,
                                            bool isDirSync,hpatch_StreamPos_t newDataSize,uint32_t kSyncBlockSize,
                                            hpatch_TChecksum* strongChecksumPlugin,hsync_TDictCompress* compressPlugin){
    const char* kVersionType=isDirSync?"HDirSynz22":"HSynz22";
    std::vector<TByte> head;
    pushTypes(head,kVersionType,compressPlugin?compressPlugin->compressType():0,strongChecksumPlugin);
    packUInt(head,newDataSize);
    packUInt(head,kSyncBlockSize);
    packUInt(head,strongChecksumPlugin->checksumByteSize());
    checkv(out_stream->write(out_stream,curOutPos,head.data(),head.data()+head.size()));
    return curOutPos+head.size();
}
static hpatch_StreamPos_t _hsynz_write_end(struct hsync_THsynz* zPlugin,
                                           const hpatch_TStreamOutput* out_stream,hpatch_StreamPos_t curOutPos,
                                           const hpatch_byte* newDataCheckChecksum,size_t checksumSize){
    checkv(out_stream->write(out_stream,curOutPos,newDataCheckChecksum,newDataCheckChecksum+checksumSize));
    return curOutPos+checksumSize;
}
void getHsynzPluginDefault(hsync_THsynz* out_hsynzPlugin){
    checkv(out_hsynzPlugin!=0);
    out_hsynzPlugin->hsynz_write_head=_hsynz_write_head;
    out_hsynzPlugin->hsynz_readed_data=0;
    out_hsynzPlugin->hsynz_write_end=_hsynz_write_end;
}


}//namespace sync_private

