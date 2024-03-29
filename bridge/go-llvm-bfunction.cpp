//===-- go-llvm-bfunction.cpp - implementation of 'Bfunction' class ---===//
//
// Copyright 2018 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.
//
//===----------------------------------------------------------------------===//
//
// Methods for class Bfunction.
//
//===----------------------------------------------------------------------===//

#include "go-llvm-bfunction.h"

#include "go-llvm-btype.h"
#include "go-llvm-bstatement.h"
#include "go-llvm-bexpression.h"
#include "go-llvm-bvariable.h"
#include "go-llvm-cabi-oracle.h"
#include "go-llvm-typemanager.h"
#include "go-llvm-irbuilders.h"
#include "go-system.h"

#include "llvm/IR/Argument.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Value.h"

Bfunction::Bfunction(llvm::Constant *fcnValue,
                     BFunctionType *fcnType,
                     const std::string &name,
                     const std::string &asmName,
                     Location location,
                     TypeManager *tm)

    : fcnType_(fcnType), fcnValue_(fcnValue),
      abiOracle_(new CABIOracle(fcnType, tm)),
      rtnValueMem_(nullptr), chainVal_(nullptr),
      paramsRegistered_(0), name_(name), asmName_(asmName),
      location_(location), splitStack_(YesSplit),
      prologGenerated_(false), abiSetupComplete_(false),
      errorSeen_(false)
{
  if (! fcnType->followsCabi())
    abiSetupComplete_ = true;
}

Bfunction::~Bfunction()
{
  if (! prologGenerated_) {
    for (auto ais : allocas_)
      ais->deleteValue();
  }
  for (auto &lab : labels_)
    delete lab;
  for (auto &v : localVariables_) {
    // only delete declvars here, others are in valueVarMap_
    // and will be deleted below.
    if (v->isDeclVar())
      delete v;
  }
  for (auto &kv : valueVarMap_)
    delete kv.second;
  assert(labelAddressPlaceholders_.empty());
}

llvm::Function *Bfunction::function() const
{
  return llvm::cast<llvm::Function>(fcnValue());
}

std::string Bfunction::namegen(const std::string &tag)
{
  return abiOracle_->tm()->tnamegen(tag);
}

llvm::Instruction *Bfunction::addAlloca(llvm::Type *typ,
                                        const std::string &name)
{
  llvm::Instruction *inst = new llvm::AllocaInst(typ, 0);
  if (! name.empty())
    inst->setName(name);
  allocas_.push_back(inst);
  return inst;
}

void Bfunction::lazyAbiSetup()
{
  if (abiSetupComplete_)
    return;
  abiSetupComplete_ = true;

  // Populate argument list
  if (arguments_.empty())
    for (auto argit = function()->arg_begin(), argen = function()->arg_end(); argit != argen; ++argit)
      arguments_.push_back(&(*argit));

  // If the return value is going to be passed via memory, make a note
  // of the argument in question, and set up the arg.
  unsigned argIdx = 0;
  if (abiOracle_->returnInfo().disp() == ParmIndirect) {
    std::string sretname(namegen("sret.formal"));
    arguments_[argIdx]->setName(sretname);
    arguments_[argIdx]->addAttr(llvm::Attribute::StructRet);
    rtnValueMem_ = arguments_[argIdx];
    argIdx += 1;
  }

  // Handle static chain param.  In contrast with real / explicit
  // function params, we don't create the spill slot eagerly.
  assert(abiOracle_->chainInfo().disp() == ParmDirect);
  std::string nestname(namegen("nest"));
  arguments_[argIdx]->setName(nestname);
  arguments_[argIdx]->addAttr(llvm::Attribute::Nest);
  chainVal_ = arguments_[argIdx];
  argIdx += 1;

  // Sort out what to do with each of the parameters.
  const std::vector<Btype *> &paramTypes = fcnType()->paramTypes();
  for (unsigned idx = 0; idx < paramTypes.size(); ++idx) {
    const CABIParamInfo &paramInfo = abiOracle_->paramInfo(idx);
    switch(paramInfo.disp()) {
      case ParmIgnore: {
        // Seems weird to create a zero-sized alloca(), but it should
        // simplify things in that we can avoid having a Bvariable with a
        // null value.
        llvm::Type *llpt = paramTypes[idx]->type();
        llvm::Instruction *inst = addAlloca(llpt, "");
        paramValues_.push_back(inst);
        break;
      }
      case ParmIndirect: {
        paramValues_.push_back(arguments_[argIdx]);
        assert(paramInfo.numArgSlots() == 1);
        arguments_[argIdx]->addAttr(llvm::Attribute::ByVal);
        argIdx += 1;
        break;
      }
      case ParmDirect: {
        llvm::Type *llpt = paramTypes[idx]->type();
        llvm::Instruction *inst = addAlloca(llpt, "");
        paramValues_.push_back(inst);
        if (paramInfo.attr() == AttrSext)
          arguments_[argIdx]->addAttr(llvm::Attribute::SExt);
        else if (paramInfo.attr() == AttrZext)
          arguments_[argIdx]->addAttr(llvm::Attribute::ZExt);
        else
          assert(paramInfo.attr() == AttrNone);
        argIdx += paramInfo.numArgSlots();
        break;
      }
    }
  }
}

Bvariable *Bfunction::parameterVariable(const std::string &name,
                                        Btype *btype,
                                        bool is_address_taken,
                                        Location location)
{
  lazyAbiSetup();
  unsigned argIdx = paramsRegistered_++;

  // Create Bvariable and install in value->var map.
  llvm::Value *argVal = paramValues_[argIdx];
  assert(valueVarMap_.find(argVal) == valueVarMap_.end());
  Bvariable *bv =
      new Bvariable(btype, location, name, ParamVar, is_address_taken, argVal);
  valueVarMap_[argVal] = bv;

  // Set parameter name or names.
  const CABIParamInfo &paramInfo = abiOracle_->paramInfo(argIdx);
  switch(paramInfo.disp()) {
    case ParmIgnore: {
      std::string iname(name);
      iname += ".ignore";
      paramValues_[argIdx]->setName(name);
      break;
    }
    case ParmIndirect: {
      unsigned soff = paramInfo.sigOffset();
      arguments_[soff]->setName(name);
      break;
    }
    case ParmDirect: {
      unsigned soff = paramInfo.sigOffset();
      if (paramInfo.abiTypes().size() == 1) {
        arguments_[soff]->setName(name);
      } else {
        assert(paramInfo.abiTypes().size() == 2);
        std::string argp1(name); argp1 += ".chunk0";
        std::string argp2(name); argp2 += ".chunk1";
        arguments_[soff]->setName(argp1);
        arguments_[soff+1]->setName(argp2);
      }
      std::string aname(name);
      aname += ".addr";
      paramValues_[argIdx]->setName(aname);
    }
  }

  // All done.
  return bv;
}

Bvariable *Bfunction::staticChainVariable(const std::string &name,
                                          Btype *btype,
                                          Location location)
{
  lazyAbiSetup();
  const CABIParamInfo &chainInfo = abiOracle_->chainInfo();
  assert(chainInfo.disp() == ParmDirect);

  // Set name of function parameter
  unsigned soff = chainInfo.sigOffset();
  arguments_[soff]->setName(name);

  // Create the spill slot for the param.
  std::string spname(name);
  spname += ".addr";
  llvm::Instruction *inst = addAlloca(btype->type(), spname);
  assert(chainVal_);
  assert(llvm::isa<llvm::Argument>(chainVal_));
  chainVal_ = inst;

  // Create backend variable to encapsulate the above.
  Bvariable *bv =
      new Bvariable(btype, location, name, ParamVar, false, inst);
  assert(valueVarMap_.find(bv->value()) == valueVarMap_.end());
  valueVarMap_[bv->value()] = bv;

  return bv;
}

Bvariable *Bfunction::localVariable(const std::string &name,
                                    Btype *btype,
                                    Bvariable *declVar,
                                    bool is_address_taken,
                                    Location location)
{
  lazyAbiSetup();
  llvm::Instruction *inst = nullptr;
  if (declVar != nullptr) {
    // If provided, declVar must be an existing local variable in
    // the same function (presumably at an outer scope).
    assert(valueVarMap_.find(declVar->value()) != valueVarMap_.end());

    // For the correct semantics, we need the two variables in question
    // to share the same alloca instruction.
    inst = llvm::cast<llvm::Instruction>(declVar->value());
  } else {
    inst = addAlloca(btype->type(), name);
  }
  Bvariable *bv =
      new Bvariable(btype, location, name, LocalVar, is_address_taken, inst);
  localVariables_.push_back(bv);
  if (declVar != nullptr) {
    // Don't add the variable in question to the value var map.
    // Mark it so that it can be handled properly during creation
    // of lifetime annotations.
    bv->markAsDeclVar();
  } else {
    assert(valueVarMap_.find(bv->value()) == valueVarMap_.end());
    valueVarMap_[bv->value()] = bv;
  }
  return bv;
}

llvm::Value *Bfunction::createTemporary(Btype *btype, const std::string &tag)
{
  return createTemporary(btype->type(), tag);
}

llvm::Value *Bfunction::createTemporary(llvm::Type *typ, const std::string &tag)
{
  return addAlloca(typ, tag);
}

// This implementation uses an alloca instruction as a placeholder
// for a block address.

llvm::Instruction *
Bfunction::createLabelAddressPlaceholder(Btype *btype)
{
  std::string name(namegen("labeladdrplaceholder"));
  llvm::Instruction *inst = new llvm::AllocaInst(btype->type(), 0);
  labelAddressPlaceholders_.insert(inst);
  return inst;
}

// Called at the point where we have a concrete basic block for
// a Blabel that has had its address taken. Replaces uses of the
// placeholder instruction with the real thing.

void Bfunction::replaceLabelAddressPlaceholder(llvm::Value *placeholder,
                                               llvm::BasicBlock *bbForLabel)
{
  // Locate the PH inst and remove it from the tracking set.
  llvm::Instruction *phinst = llvm::cast<llvm::Instruction>(placeholder);
  auto it = labelAddressPlaceholders_.find(phinst);
  assert(it != labelAddressPlaceholders_.end());
  labelAddressPlaceholders_.erase(it);

  // Create real block address and replace uses of the PH inst with it.
  llvm::BlockAddress *blockad =
      llvm::BlockAddress::get(function(), bbForLabel);
  phinst->replaceAllUsesWith(blockad);

  // Placeholder inst no longer needed.
  phinst->deleteValue();
}

std::vector<Bvariable*> Bfunction::getParameterVars()
{
  std::vector<Bvariable*> res;
  for (auto &argval : paramValues_) {
    auto it = valueVarMap_.find(argval);
    assert(it != valueVarMap_.end());
    Bvariable *v = it->second;
    res.push_back(v);
  }
  return res;
}

std::vector<Bvariable*> Bfunction::getFunctionLocalVars()
{
  return localVariables_;
}

Bvariable *Bfunction::getBvarForValue(llvm::Value *val)
{
  auto it = valueVarMap_.find(val);
  return (it != valueVarMap_.end() ? it->second : nullptr);
}

Bvariable *Bfunction::getNthParamVar(unsigned argIdx)
{
  assert(argIdx < paramValues_.size());
  llvm::Value *pval = paramValues_[argIdx];
  return getBvarForValue(pval);
}

unsigned Bfunction::genArgSpill(Bvariable *paramVar,
                                const CABIParamInfo &paramInfo,
                                Binstructions *spillInstructions,
                                llvm::Value *sploc)
{
  lazyAbiSetup();
  assert(paramInfo.disp() == ParmDirect);
  BinstructionsLIRBuilder builder(function()->getContext(), spillInstructions);
  TypeManager *tm = abiOracle_->tm();

  // Simple case: param arrived in single register.
  if (paramInfo.abiTypes().size() == 1) {
    llvm::Argument *arg = arguments_[paramInfo.sigOffset()];
    assert(sploc->getType()->isPointerTy());
    llvm::PointerType *llpt = llvm::cast<llvm::PointerType>(sploc->getType());
    if (paramInfo.abiType()->isVectorTy() ||
        arg->getType() != llpt->getElementType()) {
      std::string tag(namegen("cast"));
      llvm::Type *ptv = tm->makeLLVMPointerType(paramInfo.abiType());
      llvm::Value *bitcast = builder.CreateBitCast(sploc, ptv, tag);
      sploc = bitcast;
    }
    llvm::Instruction *si = builder.CreateStore(arg, sploc);
    paramVar->setInitializer(si);
    return 1;
  }
  assert(paramInfo.abiTypes().size() == 2);

  // More complex case: param arrives in two registers.

  // Create struct type corresponding to first and second params
  llvm::Type *llst = paramInfo.computeABIStructType(tm);
  llvm::Type *ptst = tm->makeLLVMPointerType(llst);

  // Cast the spill location to a pointer to the struct created above.
  std::string tag(namegen("cast"));
  llvm::Value *bitcast = builder.CreateBitCast(sploc, ptst, tag);

  // Generate a store to the first field
  std::string tag0(namegen("field0"));
  llvm::Value *field0gep =
      builder.CreateConstInBoundsGEP2_32(llst, bitcast, 0, 0, tag0);
  llvm::Value *argChunk0 = arguments_[paramInfo.sigOffset()];
  builder.CreateStore(argChunk0, field0gep);

  // Generate a store to the second field
  std::string tag1(namegen("field1"));
  llvm::Value *field1gep =
      builder.CreateConstInBoundsGEP2_32(llst, bitcast, 0, 1, tag0);
  llvm::Value *argChunk1 = arguments_[paramInfo.sigOffset()+1];
  llvm::Instruction *stinst = builder.CreateStore(argChunk1, field1gep);

  paramVar->setInitializer(stinst);

  // All done.
  return 2;
}

void Bfunction::genProlog(llvm::BasicBlock *entry)
{
  lazyAbiSetup();
  unsigned argIdx = (abiOracle_->returnInfo().disp() == ParmIndirect ? 1 : 0);
  Binstructions spills;

  // Spill the static chain param if needed. We only want to do this
  // if a chain variable was explicitly requested.
  const CABIParamInfo &chainInfo = abiOracle_->chainInfo();
  assert(chainInfo.disp() == ParmDirect);
  unsigned soff = chainInfo.sigOffset();
  if (arguments_[soff] != chainVal_) {
    auto it = valueVarMap_.find(chainVal_);
    assert(it != valueVarMap_.end());
    Bvariable *chainVar = it->second;
    genArgSpill(chainVar, chainInfo, &spills, chainVal_);
  }
  argIdx += 1;

  // Spill any directly-passed function arguments into their previous
  // created spill areas.
  const std::vector<Btype *> &paramTypes = fcnType()->paramTypes();
  unsigned nParms = paramTypes.size();
  for (unsigned pidx = 0; pidx < nParms; ++pidx) {
    const CABIParamInfo &paramInfo = abiOracle_->paramInfo(pidx);
    if (paramInfo.disp() != ParmDirect)
      continue;
    Bvariable *v = getNthParamVar(pidx);
    if (!v) {
      assert(errorSeen());
      continue;
    }
    llvm::Value *sploc = llvm::cast<llvm::Instruction>(paramValues_[pidx]);
    argIdx += genArgSpill(v, paramInfo, &spills, sploc);
  }

  // Append allocas for local variables
  // FIXME: create lifetime annotations
  for (auto aa : allocas_)
    entry->getInstList().push_back(aa);

  // Param spills
  for (auto sp : spills.instructions())
    entry->getInstList().push_back(sp);

  // Debug meta-data generation needs to know the position at which a
  // parameter variable is available for inspection -- this is
  // typically either A) the start of the function for by-address
  // params, or B) the spill instruction that copies a direct param to
  // the stack. If the entry block is not empty, then use the last
  // inst in it as an initializer for by-address params. If the block
  // is still empty at this point we'll take care of things later.
  if (! entry->empty()) {
    for (unsigned pidx = 0; pidx < nParms; ++pidx) {
      Bvariable *v = getNthParamVar(pidx);
      if (!v) {
        assert(errorSeen());
        continue;
      }
      if (v->initializer() == nullptr)
        v->setInitializer(&entry->front());
    }
  }

  prologGenerated_ = true;
}

void Bfunction::fixupProlog(llvm::BasicBlock *entry,
                            const std::vector<llvm::AllocaInst *> &temps)
{
  lazyAbiSetup();
  // If there are any "new" temporaries discovered during the control
  // flow generation walk, incorporate them into the entry block. At this
  // stage in the game the entry block is already fully populated, including
  // (potentially) references to the alloca instructions themselves, so
  // we insert any new temps into the start of the block.
  if (! temps.empty())
    for (auto ai : temps)
      entry->getInstList().push_front(ai);
}

llvm::Value *Bfunction::genReturnSequence(Bexpression *toRet,
                                          Binstructions *retInstrs,
                                          NameGen *inamegen)
{
  lazyAbiSetup();
  const CABIParamInfo &returnInfo = abiOracle_->returnInfo();

  // If we're returning an empty struct, or if the function has void
  // type, then return a null ptr (return "void").
  TypeManager *tm = abiOracle_->tm();
  if (returnInfo.disp() == ParmIgnore ||
      fcnType()->resultType()->type() == tm->llvmVoidType()) {
    llvm::Value *rval = nullptr;
    return rval;
  }

  // Indirect return: emit memcpy into sret
  if (returnInfo.disp() == ParmIndirect) {
    BlockLIRBuilder bbuilder(function(), inamegen);
    uint64_t sz = tm->typeSize(fcnType_->resultType());
    uint64_t algn = tm->typeAlignment(fcnType_->resultType());
    bbuilder.CreateMemCpy(rtnValueMem_, algn, toRet->value(), algn, sz);
    std::vector<llvm::Instruction*> instructions = bbuilder.instructions();
    for (auto i : instructions)
      retInstrs->appendInstruction(i);
    llvm::Value *rval = nullptr;
    return rval;
  }

  // Direct return: single value
  if (! returnInfo.abiType()->isAggregateType() &&
      ! toRet->btype()->type()->isAggregateType())
    return toRet->value();

  // Direct return: either the ABI type is a structure or the
  // return value type is a structure. In this case we bitcast
  // the return location address to the ABI type and then issue a load
  // from the bitcast.
  llvm::Type *llrt = (returnInfo.abiType()->isAggregateType() ?
                      returnInfo.computeABIStructType(tm) :
                      returnInfo.abiType());
  llvm::Type *ptst = tm->makeLLVMPointerType(llrt);
  BinstructionsLIRBuilder builder(function()->getContext(), retInstrs);
  std::string castname(namegen("cast"));
  llvm::Value *bitcast = builder.CreateBitCast(toRet->value(), ptst, castname);
  std::string loadname(namegen("ld"));
  llvm::Instruction *ldinst = builder.CreateLoad(bitcast, loadname);
  return ldinst;
}

Blabel *Bfunction::newLabel(Location loc) {
  unsigned labelCount = labels_.size();
  Blabel *lb = new Blabel(this, labelCount, loc);
  labelmap_.push_back(nullptr);
  labels_.push_back(lb);
  return lb;
}

void Bfunction::registerLabelDefStatement(Bstatement *st, Blabel *label)
{
  assert(st && st->flavor() == N_LabelStmt);
  assert(label);
  assert(labelmap_[label->label()] == nullptr);
  labelmap_[label->label()] = st;
}
