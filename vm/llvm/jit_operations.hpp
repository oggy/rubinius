#ifndef RBX_LLVM_JIT_OPERATIONS
#define RBX_LLVM_JIT_OPERATIONS

namespace rubinius {
  class JITOperations {
    llvm::Value* stack_top_;

  protected:
    LLVMState* ls_;
    llvm::BasicBlock* block_;

    llvm::Module* module_;
    llvm::Function* function_;

    llvm::Value* vm_;

  public:
    const llvm::Type* IntPtrTy;
    const llvm::Type* ObjType;
    const llvm::Type* ObjArrayTy;
    const llvm::Type* Int31Ty;

    // Frequently used types
    const llvm::Type* VMTy;
    const llvm::Type* CallFrameTy;

  public:
    JITOperations(LLVMState* ls, llvm::Module* mod, llvm::Value* top,
                  llvm::BasicBlock* start, llvm::Function* func)
      : stack_top_(top)
      , ls_(ls)
      , block_(start)
      , module_(mod)
      , function_(func)
    {
#if __LP64__
      IntPtrTy = llvm::Type::Int64Ty;
#else
      IntPtrTy = llvm::Type::Int32Ty;
#endif

      ObjType = ptr_type("Object");
      ObjArrayTy = PointerType::getUnqual(ObjType);

      Int31Ty = llvm::IntegerType::get(31);

      VMTy = ptr_type("VM");
      CallFrameTy = ptr_type("CallFrame");

      Function::arg_iterator input = function_->arg_begin();
      vm_ = input++;
    }

    LLVMState* state() {
      return ls_;
    }

    Value* vm() {
      return vm_;
    }

    // Type resolution and manipulation
    //
    const llvm::Type* ptr_type(std::string name) {
      std::string full_name = std::string("struct.rubinius::") + name;
      return PointerType::getUnqual(
          module_->getTypeByName(full_name.c_str()));
    }

    const llvm::Type* type(std::string name) {
      std::string full_name = std::string("struct.rubinius::") + name;
      return module_->getTypeByName(full_name.c_str());
    }

    Value* ptr_gep(Value* ptr, int which, const char* name, BasicBlock* block) {
      Value* idx[] = {
        ConstantInt::get(Type::Int32Ty, 0),
        ConstantInt::get(Type::Int32Ty, which)
      };

      return GetElementPtrInst::Create(ptr, idx, idx+2, name, block);
    }

    Value* upcast(Value* rec, const char* name) {
      const Type* type = ptr_type(name);

      return CastInst::Create(
          Instruction::BitCast,
          rec,
          type, "upcast", block_);
    }

    Value* check_type_bits(Value* obj, int type) {
      Value* flag_idx[] = {
        ConstantInt::get(Type::Int32Ty, 0),
        ConstantInt::get(Type::Int32Ty, 0),
        ConstantInt::get(Type::Int32Ty, 0),
        ConstantInt::get(Type::Int32Ty, 0)
      };

      Value* gep = create_gep(obj, flag_idx, 4, "flag_pos");
      Value* flags = create_load(gep, "flags");

      Value* mask = ConstantInt::get(Type::Int32Ty, (1 << 8) - 1);
      Value* obj_type = BinaryOperator::CreateAnd(flags, mask, "mask", block_);

      Value* tag = ConstantInt::get(Type::Int32Ty, type);
      return new ICmpInst(ICmpInst::ICMP_EQ, obj_type, tag, "is_tuple", block_);
    }

    // BasicBlock management
    //
    BasicBlock* current_block() {
      return block_;
    }

    BasicBlock* new_block(const char* name = "continue") {
      return BasicBlock::Create(name, function_);
    }

    void set_block(BasicBlock* bb) {
      block_ = bb;
    }

    // Stack manipulations
    //
    Value* stack_ptr(BasicBlock* block = NULL) {
      if(!block) block = block_;
      return new LoadInst(stack_top_, "stack_ptr", block);
    }

    void set_stack_ptr(Value* pos, BasicBlock* block = NULL) {
      if(!block) block = block_;
      new StoreInst(pos, stack_top_, false, block);
    }

    Value* stack_position(int amount, BasicBlock* block = NULL) {
      if(!block) block = block_;

      if(amount == 0) return stack_ptr(block);

      Value* idx = ConstantInt::get(Type::Int32Ty, amount);

      Value* stack_pos = GetElementPtrInst::Create(stack_ptr(block),
                           &idx, &idx+1, "stack_pos", block);

      return stack_pos;
    }

    Value* stack_back_position(int back, BasicBlock* block = NULL) {
      if(!block) block = block_;
      return stack_position(-back, block);
    }

    Value* stack_objects(int count, BasicBlock* block = NULL) {
      if(!block) block = block_;
      return stack_position(-(count - 1), block);
    }

    Value* stack_ptr_adjust(int amount, BasicBlock* block = NULL) {
      if(!block) block = block_;

      Value* pos = stack_position(amount, block);
      set_stack_ptr(pos, block);

      return pos;
    }

    void stack_remove(int count=1) {
      stack_ptr_adjust(-count);
    }

    void stack_push(Value* val, BasicBlock* block = NULL) {
      if(!block) block = block_;
      Value* stack_pos = stack_ptr_adjust(1, block);
      if(val->getType() == cast<PointerType>(stack_pos->getType())->getElementType()) {
        new StoreInst(val, stack_pos, false, block);
      } else {
        Value* cst = CastInst::Create(
          Instruction::BitCast,
          val,
          ObjType, "casted", block);
        new StoreInst(cst, stack_pos, false, block);
      }
    }

    llvm::Value* stack_back(int back, BasicBlock* block = NULL) {
      if(!block) block = block_;
      return new LoadInst(stack_back_position(back, block), "stack_load", block);
    }

    llvm::Value* stack_top(BasicBlock* block = NULL) {
      if(!block) block = block_;
      return stack_back(0, block);
    }

    llvm::Value* stack_pop(BasicBlock* block = NULL) {
      if(!block) block = block_;

      Value* val = stack_back(0, block);

      stack_ptr_adjust(-1, block);
      return val;
    }

    // Constant creation
    //
    Value* constant(Object* obj, BasicBlock* block = NULL) {
      if(!block) block = block_;
      return CastInst::Create(
          Instruction::IntToPtr,
          ConstantInt::get(IntPtrTy, (intptr_t)obj),
          ObjType, "cast_to_obj", block);
    }

    // numeric manipulations
    //
    Value* cast_int(Value* obj, BasicBlock* block = NULL) {
      if(!block) block = block_;

      return CastInst::Create(
          Instruction::PtrToInt,
          obj,
          IntPtrTy, "cast", block);
    }

    // Fixnum manipulations
    //
    Value* tag_strip(Value* obj, BasicBlock* block = NULL, const Type* type = NULL) {
      if(!block) block = block_;
      if(!type) type = Int31Ty;

      Value* i = CastInst::Create(
          Instruction::PtrToInt,
          obj, Type::Int32Ty, "as_int", block);

      Value* more = BinaryOperator::CreateLShr(
          i, ConstantInt::get(Type::Int32Ty, 1),
          "lshr", block);
      return CastInst::CreateIntegerCast(
          more, type, true, "stripped", block);
    }

    Value* tag_strip32(Value* obj) {
      Value* i = CastInst::Create(
          Instruction::PtrToInt,
          obj, Type::Int32Ty, "as_int", block_);

      return BinaryOperator::CreateLShr(
          i, ConstantInt::get(Type::Int32Ty, 1),
          "lshr", block_);
    }

    Value* fixnum_tag(Value* obj, BasicBlock* block = NULL) {
      if(!block) block = block_;
      Value* obj32 = CastInst::CreateZExtOrBitCast(
          obj, Type::Int32Ty, "as_32bit", block);
      Value* one = ConstantInt::get(Type::Int32Ty, 1);
      Value* more = BinaryOperator::CreateShl(obj32, one, "shl", block);
      Value* tagged = BinaryOperator::CreateOr(more, one, "or", block);

      return CastInst::Create(
          Instruction::IntToPtr, tagged, ObjType, "as_obj", block);
    }

    Value* check_if_fixnum(Value* val) {
      Value* fix_mask = ConstantInt::get(IntPtrTy, TAG_FIXNUM_MASK);
      Value* fix_tag  = ConstantInt::get(IntPtrTy, TAG_FIXNUM);

      Value* lint = cast_int(val);
      Value* masked = BinaryOperator::CreateAnd(lint, fix_mask, "masked", block_);

      return new ICmpInst(ICmpInst::ICMP_EQ, masked, fix_tag, "is_fixnum", block_);
    }

    // Tuple access
    Value* get_tuple_size(Value* tup) {
      Value* idx[] = {
        ConstantInt::get(Type::Int32Ty, 0),
        ConstantInt::get(Type::Int32Ty, offset::tuple_full_size)
      };

      Value* pos = create_gep(tup, idx, 2, "table_size_pos");

      return create_load(pos, "table_size");
    }

    // Object access
    Value* get_object_slot(Value* obj, int offset) {
      assert(offset % sizeof(Object*) == 0);

      Value* cst = CastInst::Create(
          Instruction::BitCast,
          obj,
          PointerType::getUnqual(ObjType), "obj_array", block_);

      Value* idx2[] = {
        ConstantInt::get(Type::Int32Ty, offset / sizeof(Object*))
      };

      Value* pos = create_gep(cst, idx2, 1, "field_pos");

      return create_load(pos, "field");
    }

    // Utilities for creating instructions
    //
    GetElementPtrInst* create_gep(Value* rec, Value** idx, int count,
                                  const char* name) {
      return GetElementPtrInst::Create(rec, idx, idx+count, name, block_);
    }

    LoadInst* create_load(Value* ptr, const char* name = 0) {
      return new LoadInst(ptr, "flags", block_);
    }

    void create_store(Value* val, Value* ptr) {
      new StoreInst(val, ptr, false, block_);
    }

    ICmpInst* create_icmp(ICmpInst::Predicate kind, Value* left, Value* right,
                          const char* name) {
      return new ICmpInst(kind, left, right, name, block_);
    }

    ICmpInst* create_equal(Value* left, Value* right, const char* name) {
      return create_icmp(ICmpInst::ICMP_EQ, left, right, name);
    }

    ICmpInst* create_less_than(Value* left, Value* right, const char* name) {
      return create_icmp(ICmpInst::ICMP_SLT, left, right, name);
    }

    Value* create_and(Value* left, Value* right, const char* name) {
      return BinaryOperator::CreateAnd(left, right, name, block_);
    }

    void create_conditional_branch(BasicBlock* if_true, BasicBlock* if_false, Value* cmp) {
      BranchInst::Create(if_true, if_false, cmp, block_);
    }

    void create_branch(BasicBlock* where) {
      BranchInst::Create(where, block_);
    }

    void write_barrier(Value* obj, Value* val) {
      Signature wb(ls_, ObjType);
      wb << VMTy;
      wb << ObjType;
      wb << ObjType;

      if(obj->getType() != ObjType) {
        obj = CastInst::Create(
          Instruction::BitCast,
          obj,
          ObjType, "casted", block_);
      }

      Value* call_args[] = { vm_, obj, val };
      wb.call("rbx_write_barrier", call_args, 3, "", block_);
    }
  };
}

#endif