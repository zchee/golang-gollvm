//===- llvm/tools/gollvm/unittests/BackendCore/BackendFcnTests.cpp ------===//
//
// Copyright 2018 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.
//
//===----------------------------------------------------------------------===//

#include "TestUtils.h"
#include "go-llvm-backend.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace goBackendUnitTests;

namespace {

TEST(BackendStmtTests, TestInitStmt) {

  FcnTestHarness h("foo");
  Llvm_backend *be = h.be();
  Bfunction *func = h.func();

  Btype *bi64t = be->integer_type(false, 64);
  Location loc;

  // local variable with init
  Bvariable *loc1 = be->local_variable(func, "loc1", bi64t, nullptr, true, loc);
  Bstatement *is = be->init_statement(func, loc1, mkInt64Const(be, 10));
  ASSERT_TRUE(is != nullptr);
  h.addStmt(is);
  EXPECT_EQ(repr(is), "store i64 10, i64* %loc1");

  // error handling
  Bvariable *loc2 = be->local_variable(func, "loc2", bi64t, nullptr, true, loc);
  Bstatement *bad = be->init_statement(func, loc2, be->error_expression());
  ASSERT_TRUE(bad != nullptr);
  EXPECT_EQ(bad, be->error_statement());
  Bstatement *notsobad = be->init_statement(func, loc2, nullptr);
  h.addStmt(notsobad);

  bool broken = h.finish(PreserveDebugInfo);
  EXPECT_FALSE(broken && "Module failed to verify.");
}

TEST(BackendStmtTests, TestAssignmentStmt) {
  FcnTestHarness h("foo");
  Llvm_backend *be = h.be();
  Bfunction *func = h.func();

  Btype *bi64t = be->integer_type(false, 64);
  Location loc;

  // assign a constant to a variable
  Bvariable *loc1 = h.mkLocal("loc1", bi64t);
  Bexpression *ve1 = be->var_expression(loc1, loc);
  Bstatement *as =
      be->assignment_statement(func, ve1, mkInt64Const(be, 123), loc);
  ASSERT_TRUE(as != nullptr);
  h.addStmt(as);

  // assign a variable to a variable
  Bvariable *loc2 = h.mkLocal("loc2", bi64t);
  Bexpression *ve2 = be->var_expression(loc2, loc);
  Bexpression *ve3 = be->var_expression(loc1, loc);
  Bstatement *as2 = be->assignment_statement(func, ve2, ve3, loc);
  ASSERT_TRUE(as2 != nullptr);
  h.addStmt(as2);

  const char *exp = R"RAW_RESULT(
    store i64 0, i64* %loc1
      store i64 123, i64* %loc1
      store i64 0, i64* %loc2
      %loc1.ld.0 = load i64, i64* %loc1
      store i64 %loc1.ld.0, i64* %loc2
   )RAW_RESULT";
  bool isOK = h.expectBlock(exp);
  EXPECT_TRUE(isOK && "Block does not have expected contents");

  // error handling
  Bvariable *loc3 = h.mkLocal("loc3", bi64t);
  Bexpression *ve4 = be->var_expression(loc3, loc);
  Bstatement *badas =
      be->assignment_statement(func, ve4, be->error_expression(), loc);
  ASSERT_TRUE(badas != nullptr);
  EXPECT_EQ(badas, be->error_statement());

  bool broken = h.finish(PreserveDebugInfo);
  EXPECT_FALSE(broken && "Module failed to verify.");
}

TEST(BackendStmtTests, TestReturnStmt) {

  FcnTestHarness h("foo");
  Llvm_backend *be = h.be();
  Bfunction *func = h.func();

  Btype *bi64t = be->integer_type(false, 64);
  Bvariable *loc1 = h.mkLocal("loc1", bi64t, mkInt64Const(be, 10));

  // return loc1
  Location loc;
  Bexpression *ve1 = be->var_expression(loc1, loc);
  Bstatement *ret = h.mkReturn(ve1);

  const char *exp = R"RAW_RESULT(
     %loc1.ld.0 = load i64, i64* %loc1
     ret i64 %loc1.ld.0
   )RAW_RESULT";
  std::string reason;
  bool equal = difftokens(exp, repr(ret), reason);
  EXPECT_EQ("pass", equal ? "pass" : reason);

  // error handling
  std::vector<Bexpression *> bvals;
  bvals.push_back(be->error_expression());
  Bstatement *bret = be->return_statement(func, bvals, loc);
  EXPECT_EQ(bret, be->error_statement());

  bool broken = h.finish(PreserveDebugInfo);
  EXPECT_FALSE(broken && "Module failed to verify.");
}

TEST(BackendStmtTests, TestReturnStmt2) {
  // Test that dead code after the return statement is handled
  // correctly.

  FcnTestHarness h("foo");
  Llvm_backend *be = h.be();
  Bfunction *func = h.func();
  Location loc;

  // var x int64 = 10
  Btype *bi64t = be->integer_type(false, 64);
  Bvariable *x = h.mkLocal("x", bi64t, mkInt64Const(be, 10));

  // return x
  Bexpression *ve1 = be->var_expression(x, loc);
  h.mkReturn(ve1);

  // some dead code
  // return x+20
  Bexpression *ve2 = be->var_expression(x, loc);
  Bexpression *addexpr = be->binary_expression(OPERATOR_PLUS, ve2, mkInt64Const(be, 20), loc);
  h.mkReturn(addexpr);

  const char *exp = R"RAW_RESULT(
    define i64 @foo(i8* nest %nest.0, i32 %param1, i32 %param2, i64* %param3) #0 {
    entry:
      %param1.addr = alloca i32
      %param2.addr = alloca i32
      %param3.addr = alloca i64*
      %x = alloca i64
      store i32 %param1, i32* %param1.addr
      store i32 %param2, i32* %param2.addr
      store i64* %param3, i64** %param3.addr
      store i64 10, i64* %x
      %x.ld.0 = load i64, i64* %x
      ret i64 %x.ld.0
    }
  )RAW_RESULT";

  bool broken = h.finish(StripDebugInfo);
  EXPECT_FALSE(broken && "Module failed to verify.");

  bool isOK = h.expectValue(func->function(), exp);
  EXPECT_TRUE(isOK && "Function does not have expected contents");
}

TEST(BackendStmtTests, TestLabelGotoStmts) {

  FcnTestHarness h("foo");
  Llvm_backend *be = h.be();
  Bfunction *func = h.func();

  // loc1 = 10
  Location loc;
  Btype *bi64t = be->integer_type(false, 64);
  Bvariable *loc1 = h.mkLocal("loc1", bi64t, mkInt64Const(be, 10));

  // goto labeln
  Blabel *lab1 = be->label(func, "foolab", loc);
  Bstatement *gots = be->goto_statement(lab1, loc);
  h.addStmt(gots);

  // dead stmt: loc1 = 11
  h.mkAssign(be->var_expression(loc1, loc),
             mkInt64Const(be, 11));

  // labeldef
  Bstatement *ldef = be->label_definition_statement(lab1);
  h.addStmt(ldef);

  bool broken = h.finish(PreserveDebugInfo);
  EXPECT_FALSE(broken && "Module failed to verify.");
}

TEST(BackendStmtTests, TestLabelAddressExpression) {

  FcnTestHarness h;
  Llvm_backend *be = h.be();
  BFunctionType *befty = mkFuncTyp(be, L_END);
  Bfunction *func = h.mkFunction("foo", befty);
  Btype *bu8t = be->integer_type(true, 8);
  Bvariable *loc1 = h.mkLocal("loc1", bu8t);
  Btype *bpvt = be->pointer_type(be->void_type());
  BFunctionType *befty2 = mkFuncTyp(be, L_PARM, bpvt, L_END);
  Bfunction *f2 = mkFuncFromType(be, "bar", befty2);

  // Create a label and take it's address, then pass the
  // value in question in a call to the function "bar".
  Blabel *lab1 = be->label(func, "retaddr", h.newloc());
  Bexpression *labadex = be->label_address(lab1, h.newloc());
  Bexpression *fn2ex = be->function_code_expression(f2, h.loc());
  std::vector<Bexpression *> args;
  args.push_back(labadex);
  Bexpression *call = be->call_expression(func, fn2ex, args, nullptr, h.loc());
  h.mkExprStmt(call);

  // Now define the label, throw a statement in it.
  Bstatement *ldef = be->label_definition_statement(lab1);
  h.addStmt(ldef);
  h.mkAssign(be->var_expression(loc1, h.loc()),
             be->zero_expression(bu8t));

  bool broken = h.finish(StripDebugInfo);
  EXPECT_FALSE(broken && "Module failed to verify.");

  const char *exp = R"RAW_RESULT(
    define void @foo(i8* nest %nest.0) #0 {
    entry:
      %loc1 = alloca i8
      store i8 0, i8* %loc1
      call void @bar(i8* nest undef, i8* blockaddress(@foo, %label.0))
      br label %label.0
    label.0:                                          ; preds = %entry
      store i8 0, i8* %loc1
      ret void
    }
    )RAW_RESULT";

  bool isOK = h.expectValue(func->function(), exp);
  EXPECT_TRUE(isOK && "Function does not have expected contents");
}

static Bstatement *CreateIfStmt(FcnTestHarness &h)
{
  Location loc;
  Llvm_backend *be = h.be();
  Bfunction *func = h.func();
  Btype *bi64t = be->integer_type(false, 64);
  Bvariable *loc1 = h.mkLocal("loc1", bi64t);

  // loc1 = 123
  Bexpression *ve1 = be->var_expression(loc1, loc);
  Bexpression *c123 = mkInt64Const(be, 123);
  Bstatement *as1 = be->assignment_statement(func, ve1, c123, loc);

  // loc1 = 987
  Bexpression *ve2 = be->var_expression(loc1, loc);
  Bexpression *c987 = mkInt64Const(be, 987);
  Bstatement *as2 = be->assignment_statement(func, ve2, c987, loc);

  // if true b1 else b2
  Bexpression *trueval = be->boolean_constant_expression(true);
  Bstatement *ifst = h.mkIf(trueval, as1, as2, FcnTestHarness::NoAppend);

  return ifst;
}

TEST(BackendStmtTests, TestIfStmt) {
  FcnTestHarness h("foo");
  Llvm_backend *be = h.be();
  Bfunction *func = h.func();

  Location loc;
  Bstatement *ifst = CreateIfStmt(h);

  // if true if
  Bexpression *tv2 = be->boolean_constant_expression(true);

  // loc2 = 456
  Btype *bi64t = be->integer_type(false, 64);
  Bvariable *loc2 = h.mkLocal("loc2", bi64t);
  Bexpression *ve3 = be->var_expression(loc2, loc);
  Bexpression *c456 = mkInt64Const(be, 456);
  Bstatement *as3 = be->assignment_statement(func, ve3, c456, loc);

  h.mkIf(tv2, ifst, as3);

  // return 10101
  h.mkReturn(mkInt64Const(be, 10101));

  bool broken = h.finish(StripDebugInfo);
  EXPECT_FALSE(broken && "Module failed to verify.");

  // verify
  const char *exp = R"RAW_RESULT(
    define i64 @foo(i8* nest %nest.0, i32 %param1, i32 %param2, i64* %param3) #0 {
    entry:
      %param1.addr = alloca i32
      %param2.addr = alloca i32
      %param3.addr = alloca i64*
      %loc1 = alloca i64
      %loc2 = alloca i64
      store i32 %param1, i32* %param1.addr
      store i32 %param2, i32* %param2.addr
      store i64* %param3, i64** %param3.addr
      store i64 0, i64* %loc1
      store i64 0, i64* %loc2
      br i1 true, label %then.0, label %else.0
    then.0:                                           ; preds = %entry
      br i1 true, label %then.1, label %else.1
    fallthrough.0:                                    ; preds = %else.0, %fallthrough.1
      ret i64 10101
    else.0:                                           ; preds = %entry
      store i64 456, i64* %loc2
      br label %fallthrough.0
    then.1:                                           ; preds = %then.0
      store i64 123, i64* %loc1
      br label %fallthrough.1
    fallthrough.1:                                    ; preds = %else.1, %then.1
      br label %fallthrough.0
    else.1:                                           ; preds = %then.0
      store i64 987, i64* %loc1
      br label %fallthrough.1
    }
    )RAW_RESULT";

  bool isOK = h.expectValue(func->function(), exp);
  EXPECT_TRUE(isOK && "Function does not have expected contents");
}

// Create a switch statement and append to the test harness current block.

static void CreateSwitchStmt(FcnTestHarness &h)
{
  Llvm_backend *be = h.be();
  Bfunction *func = h.func();

  Location loc;
  Btype *bi64t = be->integer_type(false, 64);
  Bvariable *loc1 = h.mkLocal("loc1", bi64t);

  // label for break
  Blabel *brklab = be->label(func, "break", loc);

  // first case
  // loc1 = loc1 / 123
  Bexpression *ve1 = be->var_expression(loc1, loc);
  Bexpression *ve1r = be->var_expression(loc1, loc);
  Bexpression *c123 = mkInt64Const(be, 123);
  Bexpression *div = be->binary_expression(OPERATOR_DIV, ve1r, c123, loc);
  Bstatement *as1 = be->assignment_statement(func, ve1, div, loc);
  Bstatement *goto1 = be->goto_statement(brklab, loc);
  Bstatement *cs1 = be->compound_statement(as1, goto1);

  // second case
  // loc1 = loc1 < 987 ? loc1 : 987 * loc1
  Bexpression *ve2 = be->var_expression(loc1, loc);
  Bexpression *ve2r = be->var_expression(loc1, loc);
  Bexpression *ve2r2 = be->var_expression(loc1, loc);
  Bexpression *ve2r3 = be->var_expression(loc1, loc);
  Bexpression *c987 = mkInt64Const(be, 987);
  Bexpression *mul = be->binary_expression(OPERATOR_MULT, c987, ve2r, loc);
  Bexpression *cmp = be->binary_expression(OPERATOR_LE, ve2r2, c987, loc);
  Bexpression *condex =
      be->conditional_expression(func, bi64t, cmp, ve2r3, mul, loc);
  Bstatement *as2 = be->assignment_statement(func, ve2, condex, loc);
  // implicit fallthrough

  // third case
  Bstatement *st3 = nullptr; // case has only a fallthough statement

  // fourth case
  // loc1 = 456
  Bexpression *ve4 = be->var_expression(loc1, loc);
  Bexpression *c456 = mkInt64Const(be, 456);
  Bstatement *as4 = be->assignment_statement(func, ve4, c456, loc);
  Bstatement *goto4 = be->goto_statement(brklab, loc);
  Bstatement *cs4 = be->compound_statement(as4, goto4);

  // Set up switch statements
  std::vector<Bstatement*> statements = {cs1, as2, st3, cs4};
  std::vector<std::vector<Bexpression*> > cases = {
    { mkInt64Const(be, 1), mkInt64Const(be, 2) },
    { mkInt64Const(be, 3), mkInt64Const(be, 4) },
    { mkInt64Const(be, 5)},
  };
  cases.push_back(std::vector<Bexpression*>()); // default

  // switch
  Bexpression *vesw = be->var_expression(loc1, loc);
  h.mkSwitch(vesw, cases, statements);

  // label definition
  Bstatement *labdef = be->label_definition_statement(brklab);
  h.addStmt(labdef);
}

TEST(BackendStmtTests, TestSwitchStmt) {
  FcnTestHarness h("foo");
  Llvm_backend *be = h.be();
  Bfunction *func = h.func();

  CreateSwitchStmt(h);

  // return 10101
  h.mkReturn(mkInt64Const(be, 10101));

  bool broken = h.finish(StripDebugInfo);
  EXPECT_FALSE(broken && "Module failed to verify.");

  // verify
  const char *exp = R"RAW_RESULT(
   define i64 @foo(i8* nest %nest.0, i32 %param1, i32 %param2, i64* %param3) #0 {
   entry:
     %param1.addr = alloca i32
     %param2.addr = alloca i32
     %param3.addr = alloca i64*
     %loc1 = alloca i64
     %tmpv.0 = alloca i64
     store i32 %param1, i32* %param1.addr
     store i32 %param2, i32* %param2.addr
     store i64* %param3, i64** %param3.addr
     store i64 0, i64* %loc1
     %loc1.ld.4 = load i64, i64* %loc1
     switch i64 %loc1.ld.4, label %default.0 [
       i64 1, label %case.0
       i64 2, label %case.0
       i64 3, label %case.1
       i64 4, label %case.1
       i64 5, label %case.2
     ]

   case.0:                                           ; preds = %entry, %entry
     %loc1.ld.0 = load i64, i64* %loc1
     %div.0 = sdiv i64 %loc1.ld.0, 123
     store i64 %div.0, i64* %loc1
     br label %label.0

   case.1:                                           ; preds = %entry, %entry
     %loc1.ld.1 = load i64, i64* %loc1
     %icmp.0 = icmp sle i64 %loc1.ld.1, 987
     %zext.0 = zext i1 %icmp.0 to i8
     %trunc.0 = trunc i8 %zext.0 to i1
     br i1 %trunc.0, label %then.0, label %else.0

   case.2:                                           ; preds = %entry, %fallthrough.0
     br label %default.0

   default.0:                                        ; preds = %entry, %case.2
     store i64 456, i64* %loc1
     br label %label.0

   label.0:                                          ; preds = %default.0, %case.0
     ret i64 10101

   then.0:                                           ; preds = %case.1
     %loc1.ld.3 = load i64, i64* %loc1
     store i64 %loc1.ld.3, i64* %tmpv.0
     br label %fallthrough.0

   fallthrough.0:                                    ; preds = %else.0, %then.0
     %tmpv.0.ld.0 = load i64, i64* %tmpv.0
     store i64 %tmpv.0.ld.0, i64* %loc1
     br label %case.2

   else.0:                                           ; preds = %case.1
     %loc1.ld.2 = load i64, i64* %loc1
     %mul.0 = mul i64 987, %loc1.ld.2
     store i64 %mul.0, i64* %tmpv.0
     br label %fallthrough.0
   }
  )RAW_RESULT";

  bool isOK = h.expectValue(func->function(), exp);
  EXPECT_TRUE(isOK && "Function does not have expected contents");
}

TEST(BackendStmtTests, TestStaticallyUnreachableStmts) {
  FcnTestHarness h("foo");
  Llvm_backend *be = h.be();

  // This test is designed to insure that there are no crashes,
  // errors, or memory leaks when the function being compiled
  // contains statically unreachable control flow.

  // Create a return, which will make what comes next unreachable.
  h.mkReturn(mkInt64Const(be, 10101));

  // Statically unreachable if and switch stmts.
  Bstatement *ifst = CreateIfStmt(h);
  h.addStmt(ifst);
  CreateSwitchStmt(h);

  // Don't check for orphan blocks; we're only interested in making
  // sure the compilation goes through.
  h.allowOrphans();

  bool broken = h.finish(PreserveDebugInfo);
  EXPECT_FALSE(broken && "Module failed to verify.");
}

static Bstatement *CreateDeferStmt(Llvm_backend *be,
                                   FcnTestHarness &h,
                                   Bfunction *func,
                                   Bvariable *loc1)
{
  Location loc;
  Btype *bi8t = be->integer_type(false, 8);

  // Declare checkdefer, deferreturn
  Btype *befty = mkFuncTyp(be, L_PARM, be->pointer_type(bi8t), L_END);
  bool is_decl = true; bool is_inl = false;
  bool is_vis = true; bool is_split = true;
  bool is_noret = false; bool is_uniqsec = false;
  Bfunction *bchkfcn = be->function(befty, "checkdefer", "checkdefer",
                                    is_vis, is_decl, is_inl, is_split,
                                    is_noret, is_uniqsec, h.newloc());
  Bfunction *bdefretfcn = be->function(befty, "deferreturn", "deferreturn",
                                       is_vis, is_decl, is_inl, is_split,
                                       is_noret, is_uniqsec, h.newloc());

  // Materialize call to deferreturn
  Bexpression *retfn = be->function_code_expression(bdefretfcn, h.newloc());
  std::vector<Bexpression *> args1;
  Bexpression *ve1 = be->var_expression(loc1, h.newloc());
  Bexpression *adve1 = be->address_expression(ve1, h.newloc());
  args1.push_back(adve1);
  Bexpression *undcall = be->call_expression(func, retfn, args1,
                                             nullptr, h.newloc());

  // Materialize call to checkdefer
  Bexpression *ckfn = be->function_code_expression(bchkfcn, h.newloc());
  std::vector<Bexpression *> args2;
  Bexpression *ve2 = be->var_expression(loc1, h.loc());
  Bexpression *adve2 = be->address_expression(ve2, h.loc());
  args2.push_back(adve2);
  Bexpression *ckdefcall = be->call_expression(func, ckfn, args2,
                                               nullptr, h.loc());

  // Defer statement based on the calls above.
  Bstatement *defer = be->function_defer_statement(func,
                                                   undcall,
                                                   ckdefcall,
                                                   h.newloc());
  return defer;
}

TEST(BackendStmtTests, TestDeferStmt) {
  FcnTestHarness h;
  Llvm_backend *be = h.be();
  BFunctionType *befty = mkFuncTyp(be, L_END);
  Bfunction *func = h.mkFunction("foo", befty);
  Btype *bi8t = be->integer_type(false, 8);
  Bvariable *loc1 = h.mkLocal("x", bi8t);

  Bstatement *defer = CreateDeferStmt(be, h, func, loc1);
  h.addStmt(defer);

  bool broken = h.finish(StripDebugInfo);
  EXPECT_FALSE(broken && "Module failed to verify.");

  const char *exp = R"RAW_RESULT(
define void @foo(i8* nest %nest.0) #0 personality i32 (i32, i32, i64, i8*, i8*)* @__gccgo_personality_v0 {
entry:
  %x = alloca i8
  store i8 0, i8* %x
  br label %finish.0

pad.0:                                            ; preds = %finish.0
  %ex.0 = landingpad { i8*, i32 }
          catch i8* null
  br label %catch.0

catch.0:                                          ; preds = %pad.0
  call void @checkdefer(i8* nest undef, i8* %x)
  br label %finish.0

finish.0:                                         ; preds = %catch.0, %entry
  invoke void @deferreturn(i8* nest undef, i8* %x)
          to label %cont.0 unwind label %pad.0

cont.0:                                           ; preds = %finish.0
  ret void
}
   )RAW_RESULT";

  bool isOK = h.expectValue(func->function(), exp);
  EXPECT_TRUE(isOK && "Function does not have expected contents");
}

TEST(BackendStmtTests, TestExceptionHandlingStmt) {
  FcnTestHarness h;
  Llvm_backend *be = h.be();
  BFunctionType *befty = mkFuncTyp(be, L_END);
  Bfunction *func = h.mkFunction("baz", befty);
  Btype *bi64t = be->integer_type(false, 64);
  Bvariable *loc1 = h.mkLocal("x", bi64t);
  BFunctionType *befty2 = mkFuncTyp(be,
                                    L_PARM, bi64t,
                                    L_RES, bi64t,
                                    L_END);

  bool is_decl = true; bool is_inl = false;
  bool is_vis = true; bool is_split = true;
  bool is_noret = false; bool is_uniqsec = false;
  const char *fnames[] = { "plark", "plix", "ohstopit" };
  Bfunction *fcns[4];
  Bexpression *calls[4];
  for (unsigned ii = 0; ii < 3; ++ii)  {
    fcns[ii] = be->function(befty, fnames[ii], fnames[ii],
                            is_vis, is_decl, is_inl, is_split,
                            is_noret, is_uniqsec, h.newloc());
    Bexpression *pfn = be->function_code_expression(fcns[ii], h.newloc());
    std::vector<Bexpression *> args;
    calls[ii] = be->call_expression(func, pfn, args,
                                    nullptr, h.newloc());
  }
  fcns[3] = be->function(befty2, "id", "id",
                         is_vis, is_decl, is_inl, is_split,
                         is_noret, is_uniqsec, h.newloc());
  Bexpression *idfn = be->function_code_expression(fcns[3], h.newloc());
  std::vector<Bexpression *> iargs;
  iargs.push_back(mkInt64Const(be, 99));
  calls[3] = be->call_expression(func, idfn, iargs,
                                 nullptr, h.newloc());

  // body:
  // x = id(99)
  // plark()
  // x = 123
  Bexpression *ve1 = be->var_expression(loc1, h.newloc());
  Bstatement *as1 =
      be->assignment_statement(func, ve1, calls[3], h.newloc());
  Bblock *bb1 = mkBlockFromStmt(be, func, as1);
  addStmtToBlock(be, bb1, h.mkExprStmt(calls[0], FcnTestHarness::NoAppend));
  Bexpression *ve2 = be->var_expression(loc1, h.newloc());
  Bstatement *as2 =
      be->assignment_statement(func, ve2, mkInt64Const(be, 123), h.newloc());
  addStmtToBlock(be, bb1, as2);
  Bstatement *body = be->block_statement(bb1);

  // catch:
  // plix()
  Bstatement *catchst = h.mkExprStmt(calls[1], FcnTestHarness::NoAppend);

  // finally:
  // ohstopit()
  Bstatement *finally = h.mkExprStmt(calls[2], FcnTestHarness::NoAppend);

  // Now exception statement
  Bstatement *est =
      be->exception_handler_statement(body, catchst, finally, h.newloc());

  h.addStmt(est);

  bool broken = h.finish(StripDebugInfo);
  EXPECT_FALSE(broken && "Module failed to verify.");

  const char *exp = R"RAW_RESULT(
define void @baz(i8* nest %nest.0) #0 personality i32 (i32, i32, i64, i8*, i8*)* @__gccgo_personality_v0 {
entry:
  %ehtmp.0 = alloca { i8*, i32 }
  %x = alloca i64
  %finvar.0 = alloca i8
  store i64 0, i64* %x
  %call.0 = invoke i64 @id(i8* nest undef, i64 99)
          to label %cont.0 unwind label %pad.0

finok.0:                                          ; preds = %cont.2, %cont.1
  store i8 1, i8* %finvar.0
  br label %finally.0

finally.0:                                        ; preds = %catchpad.0, %finok.0
  call void @ohstopit(i8* nest undef)
  %fload.0 = load i8, i8* %finvar.0
  %icmp.0 = icmp eq i8 %fload.0, 1
  br i1 %icmp.0, label %finret.0, label %finres.0

pad.0:                                            ; preds = %cont.0, %entry
  %ex.0 = landingpad { i8*, i32 }
          catch i8* null
  br label %catch.0

cont.0:                                           ; preds = %entry
  store i64 %call.0, i64* %x
  invoke void @plark(i8* nest undef)
          to label %cont.1 unwind label %pad.0

cont.1:                                           ; preds = %cont.0
  store i64 123, i64* %x
  br label %finok.0

catchpad.0:                                       ; preds = %catch.0
  %ex2.0 = landingpad { i8*, i32 }
          cleanup
  store { i8*, i32 } %ex2.0, { i8*, i32 }* %ehtmp.0
  store i8 0, i8* %finvar.0
  br label %finally.0

catch.0:                                          ; preds = %pad.0
  invoke void @plix(i8* nest undef)
          to label %cont.2 unwind label %catchpad.0

cont.2:                                           ; preds = %catch.0
  br label %finok.0

finres.0:                                         ; preds = %finally.0
  %excv.0 = load { i8*, i32 }, { i8*, i32 }* %ehtmp.0
  resume { i8*, i32 } %excv.0

finret.0:                                         ; preds = %finally.0
  ret void
}
   )RAW_RESULT";

  bool isOK = h.expectValue(func->function(), exp);
  EXPECT_TRUE(isOK && "Function does not have expected contents");
}

static Bstatement *mkMemReturn(Llvm_backend *be,
                                 Bfunction *func,
                                 Bvariable *rtmp,
                                 Bexpression *val)
{
  // Store value to tmp
  Location loc;
  Bexpression *ve = be->var_expression(rtmp, loc);
  Bstatement *as = be->assignment_statement(func, ve, val, loc);
  Bblock *block = mkBlockFromStmt(be, func, as);

  // Load from temp and return
  Bexpression *ve2 = be->var_expression(rtmp, loc);
  std::vector<Bexpression *> vals;
  vals.push_back(ve2);
  Bstatement *rst = be->return_statement(func, vals, loc);
  addStmtToBlock(be, block, rst);
  return be->block_statement(block);
}

TEST(BackendStmtTests, TestExceptionHandlingStmtWithReturns) {
  FcnTestHarness h;
  Llvm_backend *be = h.be();
  Btype *bi64t = be->integer_type(false, 64);
  BFunctionType *befty = mkFuncTyp(be,
                                    L_PARM, bi64t,
                                    L_RES, bi64t,
                                    L_END);
  Bfunction *func = h.mkFunction("baz", befty);
  Bvariable *rtmp = h.mkLocal("ret", bi64t);

  bool is_decl = true; bool is_inl = false;
  bool is_vis = true; bool is_split = true;
  bool is_noret = false; bool is_uniqsec = false;
  Bfunction *sfn = be->function(befty, "splat", "splat",
                                is_vis, is_decl, is_inl, is_split,
                                is_noret, is_uniqsec, h.newloc());
  Bexpression *splfn = be->function_code_expression(sfn, h.newloc());

  // body:
  // if splat(99) == 88 {
  //   return 22
  // } else {
  //   return parm
  // }
  Bstatement *body = nullptr;
  {
    // call to splat
    std::vector<Bexpression *> iargs;
    iargs.push_back(mkInt64Const(be, 99));
    Bexpression *splcall = be->call_expression(func, splfn, iargs,
                                               nullptr, h.newloc());
    Bexpression *eq = be->binary_expression(OPERATOR_EQEQ,
                                          splcall,
                                          mkInt64Const(be, 88),
                                          h.loc());
    Bvariable *p0 = func->getNthParamVar(0);
    Bexpression *ve1 = be->var_expression(p0, h.newloc());

    Bstatement *retparm = mkMemReturn(be, func, rtmp, ve1);
    Bstatement *ret22 = mkMemReturn(be, func, rtmp, mkInt64Const(be, 22));
    Bblock *thenblock = mkBlockFromStmt(be, func, ret22);
    Bblock *elseblock = mkBlockFromStmt(be, func, retparm);
    Bstatement *ifst = be->if_statement(func, eq,
                                        thenblock, elseblock, h.newloc());
    body = ifst;
  }

  // catch:
  // return splat(13)
  Bstatement *catchst = nullptr;
  {
    std::vector<Bexpression *> args;
    args.push_back(mkInt64Const(be, 13));
    Bexpression *splcall = be->call_expression(func, splfn, args,
                                               nullptr, h.newloc());
    catchst = mkMemReturn(be, func, rtmp, splcall);
  }

  // finally:
  // if splat(987) == 2 {
  //   return 9
  // }
  Bstatement *finally = nullptr;
  {
    std::vector<Bexpression *> args;
    args.push_back(mkInt64Const(be, 987));
    Bexpression *splcall = be->call_expression(func, splfn, args,
                                               nullptr, h.newloc());
    Bexpression *eq = be->binary_expression(OPERATOR_EQEQ,
                                            splcall,
                                            mkInt64Const(be, 2),
                                            h.loc());
    Bstatement *ret9 = mkMemReturn(be, func, rtmp, mkInt64Const(be, 9));
    Bblock *thenblock = mkBlockFromStmt(be, func, ret9);
    Bblock *elseblock = nullptr;
    Bstatement *ifst = be->if_statement(func, eq,
                                      thenblock, elseblock, h.newloc());
    finally = ifst;
  }

  // Now exception statement
  Bstatement *est =
      be->exception_handler_statement(body, catchst, finally, h.newloc());
  h.addStmt(est);

  bool broken = h.finish(StripDebugInfo);
  EXPECT_FALSE(broken && "Module failed to verify.");

  const char *exp = R"RAW_RESULT(
define i64 @baz(i8* nest %nest.0, i64 %p0) #0 personality i32 (i32, i32, i64, i8*, i8*)* @__gccgo_personality_v0 {
entry:
  %ehtmp.0 = alloca { i8*, i32 }
  %p0.addr = alloca i64
  %ret = alloca i64
  %finvar.0 = alloca i8
  store i64 %p0, i64* %p0.addr
  store i64 0, i64* %ret
  %call.0 = invoke i64 @splat(i8* nest undef, i64 99)
          to label %cont.0 unwind label %pad.0

finok.0:                                          ; preds = %cont.1, %else.0, %then.0
  store i8 1, i8* %finvar.0
  br label %finally.0

finally.0:                                        ; preds = %catchpad.0, %finok.0
  %call.2 = call i64 @splat(i8* nest undef, i64 987)
  %icmp.1 = icmp eq i64 %call.2, 2
  %zext.1 = zext i1 %icmp.1 to i8
  %trunc.1 = trunc i8 %zext.1 to i1
  br i1 %trunc.1, label %then.1, label %else.1

pad.0:                                            ; preds = %entry
  %ex.0 = landingpad { i8*, i32 }
          catch i8* null
  br label %catch.0

cont.0:                                           ; preds = %entry
  %icmp.0 = icmp eq i64 %call.0, 88
  %zext.0 = zext i1 %icmp.0 to i8
  %trunc.0 = trunc i8 %zext.0 to i1
  br i1 %trunc.0, label %then.0, label %else.0

then.0:                                           ; preds = %cont.0
  store i64 22, i64* %ret
  br label %finok.0

else.0:                                           ; preds = %cont.0
  %p0.ld.0 = load i64, i64* %p0.addr
  store i64 %p0.ld.0, i64* %ret
  br label %finok.0

catchpad.0:                                       ; preds = %catch.0
  %ex2.0 = landingpad { i8*, i32 }
          cleanup
  store { i8*, i32 } %ex2.0, { i8*, i32 }* %ehtmp.0
  store i8 0, i8* %finvar.0
  br label %finally.0

catch.0:                                          ; preds = %pad.0
  %call.1 = invoke i64 @splat(i8* nest undef, i64 13)
          to label %cont.1 unwind label %catchpad.0

cont.1:                                           ; preds = %catch.0
  store i64 %call.1, i64* %ret
  br label %finok.0

then.1:                                           ; preds = %finally.0
  store i64 9, i64* %ret
  %ret.ld.3 = load i64, i64* %ret
  ret i64 %ret.ld.3

fallthrough.1:                                    ; preds = %else.1
  %fload.0 = load i8, i8* %finvar.0
  %icmp.2 = icmp eq i8 %fload.0, 1
  br i1 %icmp.2, label %finret.0, label %finres.0

else.1:                                           ; preds = %finally.0
  br label %fallthrough.1

finres.0:                                         ; preds = %fallthrough.1
  %excv.0 = load { i8*, i32 }, { i8*, i32 }* %ehtmp.0
  resume { i8*, i32 } %excv.0

finret.0:                                         ; preds = %fallthrough.1
  %ret.ld.1 = load i64, i64* %ret
  ret i64 %ret.ld.1
}
   )RAW_RESULT";

  bool isOK = h.expectValue(func->function(), exp);
  EXPECT_TRUE(isOK && "Function does not have expected contents");
}

}
