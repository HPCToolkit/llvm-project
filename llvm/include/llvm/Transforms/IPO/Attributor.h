//===- Attributor.h --- Module-wide attribute deduction ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Attributor: An inter procedural (abstract) "attribute" deduction framework.
//
// The Attributor framework is an inter procedural abstract analysis (fixpoint
// iteration analysis). The goal is to allow easy deduction of new attributes as
// well as information exchange between abstract attributes in-flight.
//
// The Attributor class is the driver and the link between the various abstract
// attributes. The Attributor will iterate until a fixpoint state is reached by
// all abstract attributes in-flight, or until it will enforce a pessimistic fix
// point because an iteration limit is reached.
//
// Abstract attributes, derived from the AbstractAttribute class, actually
// describe properties of the code. They can correspond to actual LLVM-IR
// attributes, or they can be more general, ultimately unrelated to LLVM-IR
// attributes. The latter is useful when an abstract attributes provides
// information to other abstract attributes in-flight but we might not want to
// manifest the information. The Attributor allows to query in-flight abstract
// attributes through the `Attributor::getAAFor` method (see the method
// description for an example). If the method is used by an abstract attribute
// P, and it results in an abstract attribute Q, the Attributor will
// automatically capture a potential dependence from Q to P. This dependence
// will cause P to be reevaluated whenever Q changes in the future.
//
// The Attributor will only reevaluate abstract attributes that might have
// changed since the last iteration. That means that the Attribute will not
// revisit all instructions/blocks/functions in the module but only query
// an update from a subset of the abstract attributes.
//
// The update method `AbstractAttribute::updateImpl` is implemented by the
// specific "abstract attribute" subclasses. The method is invoked whenever the
// currently assumed state (see the AbstractState class) might not be valid
// anymore. This can, for example, happen if the state was dependent on another
// abstract attribute that changed. In every invocation, the update method has
// to adjust the internal state of an abstract attribute to a point that is
// justifiable by the underlying IR and the current state of abstract attributes
// in-flight. Since the IR is given and assumed to be valid, the information
// derived from it can be assumed to hold. However, information derived from
// other abstract attributes is conditional on various things. If the justifying
// state changed, the `updateImpl` has to revisit the situation and potentially
// find another justification or limit the optimistic assumes made.
//
// Change is the key in this framework. Until a state of no-change, thus a
// fixpoint, is reached, the Attributor will query the abstract attributes
// in-flight to re-evaluate their state. If the (current) state is too
// optimistic, hence it cannot be justified anymore through other abstract
// attributes or the state of the IR, the state of the abstract attribute will
// have to change. Generally, we assume abstract attribute state to be a finite
// height lattice and the update function to be monotone. However, these
// conditions are not enforced because the iteration limit will guarantee
// termination. If an optimistic fixpoint is reached, or a pessimistic fix
// point is enforced after a timeout, the abstract attributes are tasked to
// manifest their result in the IR for passes to come.
//
// Attribute manifestation is not mandatory. If desired, there is support to
// generate a single or multiple LLVM-IR attributes already in the helper struct
// IRAttribute. In the simplest case, a subclass inherits from IRAttribute with
// a proper Attribute::AttrKind as template parameter. The Attributor
// manifestation framework will then create and place a new attribute if it is
// allowed to do so (based on the abstract state). Other use cases can be
// achieved by overloading AbstractAttribute or IRAttribute methods.
//
//
// The "mechanics" of adding a new "abstract attribute":
// - Define a class (transitively) inheriting from AbstractAttribute and one
//   (which could be the same) that (transitively) inherits from AbstractState.
//   For the latter, consider the already available BooleanState and
//   {Inc,Dec,Bit}IntegerState if they fit your needs, e.g., you require only a
//   number tracking or bit-encoding.
// - Implement all pure methods. Also use overloading if the attribute is not
//   conforming with the "default" behavior: A (set of) LLVM-IR attribute(s) for
//   an argument, call site argument, function return value, or function. See
//   the class and method descriptions for more information on the two
//   "Abstract" classes and their respective methods.
// - Register opportunities for the new abstract attribute in the
//   `Attributor::identifyDefaultAbstractAttributes` method if it should be
//   counted as a 'default' attribute.
// - Add sufficient tests.
// - Add a Statistics object for bookkeeping. If it is a simple (set of)
//   attribute(s) manifested through the Attributor manifestation framework, see
//   the bookkeeping function in Attributor.cpp.
// - If instructions with a certain opcode are interesting to the attribute, add
//   that opcode to the switch in `Attributor::identifyAbstractAttributes`. This
//   will make it possible to query all those instructions through the
//   `InformationCache::getOpcodeInstMapForFunction` interface and eliminate the
//   need to traverse the IR repeatedly.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_IPO_ATTRIBUTOR_H
#define LLVM_TRANSFORMS_IPO_ATTRIBUTOR_H

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/GraphTraits.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/Analysis/AssumeBundleQueries.h"
#include "llvm/Analysis/CFG.h"
#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Analysis/LazyCallGraph.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/MustExecute.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/AbstractCallSite.h"
#include "llvm/IR/ConstantRange.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/TimeProfiler.h"
#include "llvm/Transforms/Utils/CallGraphUpdater.h"

namespace llvm {

struct AADepGraphNode;
struct AADepGraph;
struct Attributor;
struct AbstractAttribute;
struct InformationCache;
struct AAIsDead;

class AAManager;
class AAResults;
class Function;

/// The value passed to the line option that defines the maximal initialization
/// chain length.
extern unsigned MaxInitializationChainLength;

///{
enum class ChangeStatus {
  CHANGED,
  UNCHANGED,
};

ChangeStatus operator|(ChangeStatus l, ChangeStatus r);
ChangeStatus operator&(ChangeStatus l, ChangeStatus r);

enum class DepClassTy {
  REQUIRED, ///< The target cannot be valid if the source is not.
  OPTIONAL, ///< The target may be valid if the source is not.
  NONE,     ///< Do not track a dependence between source and target.
};
///}

/// The data structure for the nodes of a dependency graph
struct AADepGraphNode {
public:
  virtual ~AADepGraphNode(){};
  using DepTy = PointerIntPair<AADepGraphNode *, 1>;

protected:
  /// Set of dependency graph nodes which should be updated if this one
  /// is updated. The bit encodes if it is optional.
  TinyPtrVector<DepTy> Deps;

  static AADepGraphNode *DepGetVal(DepTy &DT) { return DT.getPointer(); }
  static AbstractAttribute *DepGetValAA(DepTy &DT) {
    return cast<AbstractAttribute>(DT.getPointer());
  }

  operator AbstractAttribute *() { return cast<AbstractAttribute>(this); }

public:
  using iterator =
      mapped_iterator<TinyPtrVector<DepTy>::iterator, decltype(&DepGetVal)>;
  using aaiterator =
      mapped_iterator<TinyPtrVector<DepTy>::iterator, decltype(&DepGetValAA)>;

  aaiterator begin() { return aaiterator(Deps.begin(), &DepGetValAA); }
  aaiterator end() { return aaiterator(Deps.end(), &DepGetValAA); }
  iterator child_begin() { return iterator(Deps.begin(), &DepGetVal); }
  iterator child_end() { return iterator(Deps.end(), &DepGetVal); }

  virtual void print(raw_ostream &OS) const { OS << "AADepNode Impl\n"; }
  TinyPtrVector<DepTy> &getDeps() { return Deps; }

  friend struct Attributor;
  friend struct AADepGraph;
};

/// The data structure for the dependency graph
///
/// Note that in this graph if there is an edge from A to B (A -> B),
/// then it means that B depends on A, and when the state of A is
/// updated, node B should also be updated
struct AADepGraph {
  AADepGraph() {}
  ~AADepGraph() {}

  using DepTy = AADepGraphNode::DepTy;
  static AADepGraphNode *DepGetVal(DepTy &DT) { return DT.getPointer(); }
  using iterator =
      mapped_iterator<TinyPtrVector<DepTy>::iterator, decltype(&DepGetVal)>;

  /// There is no root node for the dependency graph. But the SCCIterator
  /// requires a single entry point, so we maintain a fake("synthetic") root
  /// node that depends on every node.
  AADepGraphNode SyntheticRoot;
  AADepGraphNode *GetEntryNode() { return &SyntheticRoot; }

  iterator begin() { return SyntheticRoot.child_begin(); }
  iterator end() { return SyntheticRoot.child_end(); }

  void viewGraph();

  /// Dump graph to file
  void dumpGraph();

  /// Print dependency graph
  void print();
};

/// Helper to describe and deal with positions in the LLVM-IR.
///
/// A position in the IR is described by an anchor value and an "offset" that
/// could be the argument number, for call sites and arguments, or an indicator
/// of the "position kind". The kinds, specified in the Kind enum below, include
/// the locations in the attribute list, i.a., function scope and return value,
/// as well as a distinction between call sites and functions. Finally, there
/// are floating values that do not have a corresponding attribute list
/// position.
struct IRPosition {
  // NOTE: In the future this definition can be changed to support recursive
  // functions.
  using CallBaseContext = CallBase;

  /// The positions we distinguish in the IR.
  enum Kind : char {
    IRP_INVALID,  ///< An invalid position.
    IRP_FLOAT,    ///< A position that is not associated with a spot suitable
                  ///< for attributes. This could be any value or instruction.
    IRP_RETURNED, ///< An attribute for the function return value.
    IRP_CALL_SITE_RETURNED, ///< An attribute for a call site return value.
    IRP_FUNCTION,           ///< An attribute for a function (scope).
    IRP_CALL_SITE,          ///< An attribute for a call site (function scope).
    IRP_ARGUMENT,           ///< An attribute for a function argument.
    IRP_CALL_SITE_ARGUMENT, ///< An attribute for a call site argument.
  };

  /// Default constructor available to create invalid positions implicitly. All
  /// other positions need to be created explicitly through the appropriate
  /// static member function.
  IRPosition() : Enc(nullptr, ENC_VALUE) { verify(); }

  /// Create a position describing the value of \p V.
  static const IRPosition value(const Value &V,
                                const CallBaseContext *CBContext = nullptr) {
    if (auto *Arg = dyn_cast<Argument>(&V))
      return IRPosition::argument(*Arg, CBContext);
    if (auto *CB = dyn_cast<CallBase>(&V))
      return IRPosition::callsite_returned(*CB);
    return IRPosition(const_cast<Value &>(V), IRP_FLOAT, CBContext);
  }

  /// Create a position describing the function scope of \p F.
  /// \p CBContext is used for call base specific analysis.
  static const IRPosition function(const Function &F,
                                   const CallBaseContext *CBContext = nullptr) {
    return IRPosition(const_cast<Function &>(F), IRP_FUNCTION, CBContext);
  }

  /// Create a position describing the returned value of \p F.
  /// \p CBContext is used for call base specific analysis.
  static const IRPosition returned(const Function &F,
                                   const CallBaseContext *CBContext = nullptr) {
    return IRPosition(const_cast<Function &>(F), IRP_RETURNED, CBContext);
  }

  /// Create a position describing the argument \p Arg.
  /// \p CBContext is used for call base specific analysis.
  static const IRPosition argument(const Argument &Arg,
                                   const CallBaseContext *CBContext = nullptr) {
    return IRPosition(const_cast<Argument &>(Arg), IRP_ARGUMENT, CBContext);
  }

  /// Create a position describing the function scope of \p CB.
  static const IRPosition callsite_function(const CallBase &CB) {
    return IRPosition(const_cast<CallBase &>(CB), IRP_CALL_SITE);
  }

  /// Create a position describing the returned value of \p CB.
  static const IRPosition callsite_returned(const CallBase &CB) {
    return IRPosition(const_cast<CallBase &>(CB), IRP_CALL_SITE_RETURNED);
  }

  /// Create a position describing the argument of \p CB at position \p ArgNo.
  static const IRPosition callsite_argument(const CallBase &CB,
                                            unsigned ArgNo) {
    return IRPosition(const_cast<Use &>(CB.getArgOperandUse(ArgNo)),
                      IRP_CALL_SITE_ARGUMENT);
  }

  /// Create a position describing the argument of \p ACS at position \p ArgNo.
  static const IRPosition callsite_argument(AbstractCallSite ACS,
                                            unsigned ArgNo) {
    if (ACS.getNumArgOperands() <= ArgNo)
      return IRPosition();
    int CSArgNo = ACS.getCallArgOperandNo(ArgNo);
    if (CSArgNo >= 0)
      return IRPosition::callsite_argument(
          cast<CallBase>(*ACS.getInstruction()), CSArgNo);
    return IRPosition();
  }

  /// Create a position with function scope matching the "context" of \p IRP.
  /// If \p IRP is a call site (see isAnyCallSitePosition()) then the result
  /// will be a call site position, otherwise the function position of the
  /// associated function.
  static const IRPosition
  function_scope(const IRPosition &IRP,
                 const CallBaseContext *CBContext = nullptr) {
    if (IRP.isAnyCallSitePosition()) {
      return IRPosition::callsite_function(
          cast<CallBase>(IRP.getAnchorValue()));
    }
    assert(IRP.getAssociatedFunction());
    return IRPosition::function(*IRP.getAssociatedFunction(), CBContext);
  }

  bool operator==(const IRPosition &RHS) const {
    return Enc == RHS.Enc && RHS.CBContext == CBContext;
  }
  bool operator!=(const IRPosition &RHS) const { return !(*this == RHS); }

  /// Return the value this abstract attribute is anchored with.
  ///
  /// The anchor value might not be the associated value if the latter is not
  /// sufficient to determine where arguments will be manifested. This is, so
  /// far, only the case for call site arguments as the value is not sufficient
  /// to pinpoint them. Instead, we can use the call site as an anchor.
  Value &getAnchorValue() const {
    switch (getEncodingBits()) {
    case ENC_VALUE:
    case ENC_RETURNED_VALUE:
    case ENC_FLOATING_FUNCTION:
      return *getAsValuePtr();
    case ENC_CALL_SITE_ARGUMENT_USE:
      return *(getAsUsePtr()->getUser());
    default:
      llvm_unreachable("Unkown encoding!");
    };
  }

  /// Return the associated function, if any.
  Function *getAssociatedFunction() const {
    if (auto *CB = dyn_cast<CallBase>(&getAnchorValue())) {
      // We reuse the logic that associates callback calles to arguments of a
      // call site here to identify the callback callee as the associated
      // function.
      if (Argument *Arg = getAssociatedArgument())
        return Arg->getParent();
      return CB->getCalledFunction();
    }
    return getAnchorScope();
  }

  /// Return the associated argument, if any.
  Argument *getAssociatedArgument() const;

  /// Return true if the position refers to a function interface, that is the
  /// function scope, the function return, or an argument.
  bool isFnInterfaceKind() const {
    switch (getPositionKind()) {
    case IRPosition::IRP_FUNCTION:
    case IRPosition::IRP_RETURNED:
    case IRPosition::IRP_ARGUMENT:
      return true;
    default:
      return false;
    }
  }

  /// Return the Function surrounding the anchor value.
  Function *getAnchorScope() const {
    Value &V = getAnchorValue();
    if (isa<Function>(V))
      return &cast<Function>(V);
    if (isa<Argument>(V))
      return cast<Argument>(V).getParent();
    if (isa<Instruction>(V))
      return cast<Instruction>(V).getFunction();
    return nullptr;
  }

  /// Return the context instruction, if any.
  Instruction *getCtxI() const {
    Value &V = getAnchorValue();
    if (auto *I = dyn_cast<Instruction>(&V))
      return I;
    if (auto *Arg = dyn_cast<Argument>(&V))
      if (!Arg->getParent()->isDeclaration())
        return &Arg->getParent()->getEntryBlock().front();
    if (auto *F = dyn_cast<Function>(&V))
      if (!F->isDeclaration())
        return &(F->getEntryBlock().front());
    return nullptr;
  }

  /// Return the value this abstract attribute is associated with.
  Value &getAssociatedValue() const {
    if (getCallSiteArgNo() < 0 || isa<Argument>(&getAnchorValue()))
      return getAnchorValue();
    assert(isa<CallBase>(&getAnchorValue()) && "Expected a call base!");
    return *cast<CallBase>(&getAnchorValue())
                ->getArgOperand(getCallSiteArgNo());
  }

  /// Return the type this abstract attribute is associated with.
  Type *getAssociatedType() const {
    if (getPositionKind() == IRPosition::IRP_RETURNED)
      return getAssociatedFunction()->getReturnType();
    return getAssociatedValue().getType();
  }

  /// Return the callee argument number of the associated value if it is an
  /// argument or call site argument, otherwise a negative value. In contrast to
  /// `getCallSiteArgNo` this method will always return the "argument number"
  /// from the perspective of the callee. This may not the same as the call site
  /// if this is a callback call.
  int getCalleeArgNo() const {
    return getArgNo(/* CallbackCalleeArgIfApplicable */ true);
  }

  /// Return the call site argument number of the associated value if it is an
  /// argument or call site argument, otherwise a negative value. In contrast to
  /// `getCalleArgNo` this method will always return the "operand number" from
  /// the perspective of the call site. This may not the same as the callee
  /// perspective if this is a callback call.
  int getCallSiteArgNo() const {
    return getArgNo(/* CallbackCalleeArgIfApplicable */ false);
  }

  /// Return the index in the attribute list for this position.
  unsigned getAttrIdx() const {
    switch (getPositionKind()) {
    case IRPosition::IRP_INVALID:
    case IRPosition::IRP_FLOAT:
      break;
    case IRPosition::IRP_FUNCTION:
    case IRPosition::IRP_CALL_SITE:
      return AttributeList::FunctionIndex;
    case IRPosition::IRP_RETURNED:
    case IRPosition::IRP_CALL_SITE_RETURNED:
      return AttributeList::ReturnIndex;
    case IRPosition::IRP_ARGUMENT:
    case IRPosition::IRP_CALL_SITE_ARGUMENT:
      return getCallSiteArgNo() + AttributeList::FirstArgIndex;
    }
    llvm_unreachable(
        "There is no attribute index for a floating or invalid position!");
  }

  /// Return the associated position kind.
  Kind getPositionKind() const {
    char EncodingBits = getEncodingBits();
    if (EncodingBits == ENC_CALL_SITE_ARGUMENT_USE)
      return IRP_CALL_SITE_ARGUMENT;
    if (EncodingBits == ENC_FLOATING_FUNCTION)
      return IRP_FLOAT;

    Value *V = getAsValuePtr();
    if (!V)
      return IRP_INVALID;
    if (isa<Argument>(V))
      return IRP_ARGUMENT;
    if (isa<Function>(V))
      return isReturnPosition(EncodingBits) ? IRP_RETURNED : IRP_FUNCTION;
    if (isa<CallBase>(V))
      return isReturnPosition(EncodingBits) ? IRP_CALL_SITE_RETURNED
                                            : IRP_CALL_SITE;
    return IRP_FLOAT;
  }

  /// TODO: Figure out if the attribute related helper functions should live
  ///       here or somewhere else.

  /// Return true if any kind in \p AKs existing in the IR at a position that
  /// will affect this one. See also getAttrs(...).
  /// \param IgnoreSubsumingPositions Flag to determine if subsuming positions,
  ///                                 e.g., the function position if this is an
  ///                                 argument position, should be ignored.
  bool hasAttr(ArrayRef<Attribute::AttrKind> AKs,
               bool IgnoreSubsumingPositions = false,
               Attributor *A = nullptr) const;

  /// Return the attributes of any kind in \p AKs existing in the IR at a
  /// position that will affect this one. While each position can only have a
  /// single attribute of any kind in \p AKs, there are "subsuming" positions
  /// that could have an attribute as well. This method returns all attributes
  /// found in \p Attrs.
  /// \param IgnoreSubsumingPositions Flag to determine if subsuming positions,
  ///                                 e.g., the function position if this is an
  ///                                 argument position, should be ignored.
  void getAttrs(ArrayRef<Attribute::AttrKind> AKs,
                SmallVectorImpl<Attribute> &Attrs,
                bool IgnoreSubsumingPositions = false,
                Attributor *A = nullptr) const;

  /// Remove the attribute of kind \p AKs existing in the IR at this position.
  void removeAttrs(ArrayRef<Attribute::AttrKind> AKs) const {
    if (getPositionKind() == IRP_INVALID || getPositionKind() == IRP_FLOAT)
      return;

    AttributeList AttrList;
    auto *CB = dyn_cast<CallBase>(&getAnchorValue());
    if (CB)
      AttrList = CB->getAttributes();
    else
      AttrList = getAssociatedFunction()->getAttributes();

    LLVMContext &Ctx = getAnchorValue().getContext();
    for (Attribute::AttrKind AK : AKs)
      AttrList = AttrList.removeAttribute(Ctx, getAttrIdx(), AK);

    if (CB)
      CB->setAttributes(AttrList);
    else
      getAssociatedFunction()->setAttributes(AttrList);
  }

  bool isAnyCallSitePosition() const {
    switch (getPositionKind()) {
    case IRPosition::IRP_CALL_SITE:
    case IRPosition::IRP_CALL_SITE_RETURNED:
    case IRPosition::IRP_CALL_SITE_ARGUMENT:
      return true;
    default:
      return false;
    }
  }

  /// Return true if the position is an argument or call site argument.
  bool isArgumentPosition() const {
    switch (getPositionKind()) {
    case IRPosition::IRP_ARGUMENT:
    case IRPosition::IRP_CALL_SITE_ARGUMENT:
      return true;
    default:
      return false;
    }
  }

  /// Return the same position without the call base context.
  IRPosition stripCallBaseContext() const {
    IRPosition Result = *this;
    Result.CBContext = nullptr;
    return Result;
  }

  /// Get the call base context from the position.
  const CallBaseContext *getCallBaseContext() const { return CBContext; }

  /// Check if the position has any call base context.
  bool hasCallBaseContext() const { return CBContext != nullptr; }

  /// Special DenseMap key values.
  ///
  ///{
  static const IRPosition EmptyKey;
  static const IRPosition TombstoneKey;
  ///}

  /// Conversion into a void * to allow reuse of pointer hashing.
  operator void *() const { return Enc.getOpaqueValue(); }

private:
  /// Private constructor for special values only!
  explicit IRPosition(void *Ptr, const CallBaseContext *CBContext = nullptr)
      : CBContext(CBContext) {
    Enc.setFromOpaqueValue(Ptr);
  }

  /// IRPosition anchored at \p AnchorVal with kind/argument numbet \p PK.
  explicit IRPosition(Value &AnchorVal, Kind PK,
                      const CallBaseContext *CBContext = nullptr)
      : CBContext(CBContext) {
    switch (PK) {
    case IRPosition::IRP_INVALID:
      llvm_unreachable("Cannot create invalid IRP with an anchor value!");
      break;
    case IRPosition::IRP_FLOAT:
      // Special case for floating functions.
      if (isa<Function>(AnchorVal))
        Enc = {&AnchorVal, ENC_FLOATING_FUNCTION};
      else
        Enc = {&AnchorVal, ENC_VALUE};
      break;
    case IRPosition::IRP_FUNCTION:
    case IRPosition::IRP_CALL_SITE:
      Enc = {&AnchorVal, ENC_VALUE};
      break;
    case IRPosition::IRP_RETURNED:
    case IRPosition::IRP_CALL_SITE_RETURNED:
      Enc = {&AnchorVal, ENC_RETURNED_VALUE};
      break;
    case IRPosition::IRP_ARGUMENT:
      Enc = {&AnchorVal, ENC_VALUE};
      break;
    case IRPosition::IRP_CALL_SITE_ARGUMENT:
      llvm_unreachable(
          "Cannot create call site argument IRP with an anchor value!");
      break;
    }
    verify();
  }

  /// Return the callee argument number of the associated value if it is an
  /// argument or call site argument. See also `getCalleeArgNo` and
  /// `getCallSiteArgNo`.
  int getArgNo(bool CallbackCalleeArgIfApplicable) const {
    if (CallbackCalleeArgIfApplicable)
      if (Argument *Arg = getAssociatedArgument())
        return Arg->getArgNo();
    switch (getPositionKind()) {
    case IRPosition::IRP_ARGUMENT:
      return cast<Argument>(getAsValuePtr())->getArgNo();
    case IRPosition::IRP_CALL_SITE_ARGUMENT: {
      Use &U = *getAsUsePtr();
      return cast<CallBase>(U.getUser())->getArgOperandNo(&U);
    }
    default:
      return -1;
    }
  }

  /// IRPosition for the use \p U. The position kind \p PK needs to be
  /// IRP_CALL_SITE_ARGUMENT, the anchor value is the user, the associated value
  /// the used value.
  explicit IRPosition(Use &U, Kind PK) {
    assert(PK == IRP_CALL_SITE_ARGUMENT &&
           "Use constructor is for call site arguments only!");
    Enc = {&U, ENC_CALL_SITE_ARGUMENT_USE};
    verify();
  }

  /// Verify internal invariants.
  void verify();

  /// Return the attributes of kind \p AK existing in the IR as attribute.
  bool getAttrsFromIRAttr(Attribute::AttrKind AK,
                          SmallVectorImpl<Attribute> &Attrs) const;

  /// Return the attributes of kind \p AK existing in the IR as operand bundles
  /// of an llvm.assume.
  bool getAttrsFromAssumes(Attribute::AttrKind AK,
                           SmallVectorImpl<Attribute> &Attrs,
                           Attributor &A) const;

  /// Return the underlying pointer as Value *, valid for all positions but
  /// IRP_CALL_SITE_ARGUMENT.
  Value *getAsValuePtr() const {
    assert(getEncodingBits() != ENC_CALL_SITE_ARGUMENT_USE &&
           "Not a value pointer!");
    return reinterpret_cast<Value *>(Enc.getPointer());
  }

  /// Return the underlying pointer as Use *, valid only for
  /// IRP_CALL_SITE_ARGUMENT positions.
  Use *getAsUsePtr() const {
    assert(getEncodingBits() == ENC_CALL_SITE_ARGUMENT_USE &&
           "Not a value pointer!");
    return reinterpret_cast<Use *>(Enc.getPointer());
  }

  /// Return true if \p EncodingBits describe a returned or call site returned
  /// position.
  static bool isReturnPosition(char EncodingBits) {
    return EncodingBits == ENC_RETURNED_VALUE;
  }

  /// Return true if the encoding bits describe a returned or call site returned
  /// position.
  bool isReturnPosition() const { return isReturnPosition(getEncodingBits()); }

  /// The encoding of the IRPosition is a combination of a pointer and two
  /// encoding bits. The values of the encoding bits are defined in the enum
  /// below. The pointer is either a Value* (for the first three encoding bit
  /// combinations) or Use* (for ENC_CALL_SITE_ARGUMENT_USE).
  ///
  ///{
  enum {
    ENC_VALUE = 0b00,
    ENC_RETURNED_VALUE = 0b01,
    ENC_FLOATING_FUNCTION = 0b10,
    ENC_CALL_SITE_ARGUMENT_USE = 0b11,
  };

  // Reserve the maximal amount of bits so there is no need to mask out the
  // remaining ones. We will not encode anything else in the pointer anyway.
  static constexpr int NumEncodingBits =
      PointerLikeTypeTraits<void *>::NumLowBitsAvailable;
  static_assert(NumEncodingBits >= 2, "At least two bits are required!");

  /// The pointer with the encoding bits.
  PointerIntPair<void *, NumEncodingBits, char> Enc;
  ///}

  /// Call base context. Used for callsite specific analysis.
  const CallBaseContext *CBContext = nullptr;

  /// Return the encoding bits.
  char getEncodingBits() const { return Enc.getInt(); }
};

/// Helper that allows IRPosition as a key in a DenseMap.
template <> struct DenseMapInfo<IRPosition> {
  static inline IRPosition getEmptyKey() { return IRPosition::EmptyKey; }
  static inline IRPosition getTombstoneKey() {
    return IRPosition::TombstoneKey;
  }
  static unsigned getHashValue(const IRPosition &IRP) {
    return (DenseMapInfo<void *>::getHashValue(IRP) << 4) ^
           (DenseMapInfo<Value *>::getHashValue(IRP.getCallBaseContext()));
  }

  static bool isEqual(const IRPosition &a, const IRPosition &b) {
    return a == b;
  }
};

/// A visitor class for IR positions.
///
/// Given a position P, the SubsumingPositionIterator allows to visit "subsuming
/// positions" wrt. attributes/information. Thus, if a piece of information
/// holds for a subsuming position, it also holds for the position P.
///
/// The subsuming positions always include the initial position and then,
/// depending on the position kind, additionally the following ones:
/// - for IRP_RETURNED:
///   - the function (IRP_FUNCTION)
/// - for IRP_ARGUMENT:
///   - the function (IRP_FUNCTION)
/// - for IRP_CALL_SITE:
///   - the callee (IRP_FUNCTION), if known
/// - for IRP_CALL_SITE_RETURNED:
///   - the callee (IRP_RETURNED), if known
///   - the call site (IRP_FUNCTION)
///   - the callee (IRP_FUNCTION), if known
/// - for IRP_CALL_SITE_ARGUMENT:
///   - the argument of the callee (IRP_ARGUMENT), if known
///   - the callee (IRP_FUNCTION), if known
///   - the position the call site argument is associated with if it is not
///     anchored to the call site, e.g., if it is an argument then the argument
///     (IRP_ARGUMENT)
class SubsumingPositionIterator {
  SmallVector<IRPosition, 4> IRPositions;
  using iterator = decltype(IRPositions)::iterator;

public:
  SubsumingPositionIterator(const IRPosition &IRP);
  iterator begin() { return IRPositions.begin(); }
  iterator end() { return IRPositions.end(); }
};

/// Wrapper for FunctoinAnalysisManager.
struct AnalysisGetter {
  template <typename Analysis>
  typename Analysis::Result *getAnalysis(const Function &F) {
    if (!FAM || !F.getParent())
      return nullptr;
    return &FAM->getResult<Analysis>(const_cast<Function &>(F));
  }

  AnalysisGetter(FunctionAnalysisManager &FAM) : FAM(&FAM) {}
  AnalysisGetter() {}

private:
  FunctionAnalysisManager *FAM = nullptr;
};

/// Data structure to hold cached (LLVM-IR) information.
///
/// All attributes are given an InformationCache object at creation time to
/// avoid inspection of the IR by all of them individually. This default
/// InformationCache will hold information required by 'default' attributes,
/// thus the ones deduced when Attributor::identifyDefaultAbstractAttributes(..)
/// is called.
///
/// If custom abstract attributes, registered manually through
/// Attributor::registerAA(...), need more information, especially if it is not
/// reusable, it is advised to inherit from the InformationCache and cast the
/// instance down in the abstract attributes.
struct InformationCache {
  InformationCache(const Module &M, AnalysisGetter &AG,
                   BumpPtrAllocator &Allocator, SetVector<Function *> *CGSCC)
      : DL(M.getDataLayout()), Allocator(Allocator),
        Explorer(
            /* ExploreInterBlock */ true, /* ExploreCFGForward */ true,
            /* ExploreCFGBackward */ true,
            /* LIGetter */
            [&](const Function &F) { return AG.getAnalysis<LoopAnalysis>(F); },
            /* DTGetter */
            [&](const Function &F) {
              return AG.getAnalysis<DominatorTreeAnalysis>(F);
            },
            /* PDTGetter */
            [&](const Function &F) {
              return AG.getAnalysis<PostDominatorTreeAnalysis>(F);
            }),
        AG(AG), CGSCC(CGSCC) {
    if (CGSCC)
      initializeModuleSlice(*CGSCC);
  }

  ~InformationCache() {
    // The FunctionInfo objects are allocated via a BumpPtrAllocator, we call
    // the destructor manually.
    for (auto &It : FuncInfoMap)
      It.getSecond()->~FunctionInfo();
  }

  /// Apply \p CB to all uses of \p F. If \p LookThroughConstantExprUses is
  /// true, constant expression users are not given to \p CB but their uses are
  /// traversed transitively.
  template <typename CBTy>
  static void foreachUse(Function &F, CBTy CB,
                         bool LookThroughConstantExprUses = true) {
    SmallVector<Use *, 8> Worklist(make_pointer_range(F.uses()));

    for (unsigned Idx = 0; Idx < Worklist.size(); ++Idx) {
      Use &U = *Worklist[Idx];

      // Allow use in constant bitcasts and simply look through them.
      if (LookThroughConstantExprUses && isa<ConstantExpr>(U.getUser())) {
        for (Use &CEU : cast<ConstantExpr>(U.getUser())->uses())
          Worklist.push_back(&CEU);
        continue;
      }

      CB(U);
    }
  }

  /// Initialize the ModuleSlice member based on \p SCC. ModuleSlices contains
  /// (a subset of) all functions that we can look at during this SCC traversal.
  /// This includes functions (transitively) called from the SCC and the
  /// (transitive) callers of SCC functions. We also can look at a function if
  /// there is a "reference edge", i.a., if the function somehow uses (!=calls)
  /// a function in the SCC or a caller of a function in the SCC.
  void initializeModuleSlice(SetVector<Function *> &SCC) {
    ModuleSlice.insert(SCC.begin(), SCC.end());

    SmallPtrSet<Function *, 16> Seen;
    SmallVector<Function *, 16> Worklist(SCC.begin(), SCC.end());
    while (!Worklist.empty()) {
      Function *F = Worklist.pop_back_val();
      ModuleSlice.insert(F);

      for (Instruction &I : instructions(*F))
        if (auto *CB = dyn_cast<CallBase>(&I))
          if (Function *Callee = CB->getCalledFunction())
            if (Seen.insert(Callee).second)
              Worklist.push_back(Callee);
    }

    Seen.clear();
    Worklist.append(SCC.begin(), SCC.end());
    while (!Worklist.empty()) {
      Function *F = Worklist.pop_back_val();
      ModuleSlice.insert(F);

      // Traverse all transitive uses.
      foreachUse(*F, [&](Use &U) {
        if (auto *UsrI = dyn_cast<Instruction>(U.getUser()))
          if (Seen.insert(UsrI->getFunction()).second)
            Worklist.push_back(UsrI->getFunction());
      });
    }
  }

  /// The slice of the module we are allowed to look at.
  SmallPtrSet<Function *, 8> ModuleSlice;

  /// A vector type to hold instructions.
  using InstructionVectorTy = SmallVector<Instruction *, 8>;

  /// A map type from opcodes to instructions with this opcode.
  using OpcodeInstMapTy = DenseMap<unsigned, InstructionVectorTy *>;

  /// Return the map that relates "interesting" opcodes with all instructions
  /// with that opcode in \p F.
  OpcodeInstMapTy &getOpcodeInstMapForFunction(const Function &F) {
    return getFunctionInfo(F).OpcodeInstMap;
  }

  /// Return the instructions in \p F that may read or write memory.
  InstructionVectorTy &getReadOrWriteInstsForFunction(const Function &F) {
    return getFunctionInfo(F).RWInsts;
  }

  /// Return MustBeExecutedContextExplorer
  MustBeExecutedContextExplorer &getMustBeExecutedContextExplorer() {
    return Explorer;
  }

  /// Return TargetLibraryInfo for function \p F.
  TargetLibraryInfo *getTargetLibraryInfoForFunction(const Function &F) {
    return AG.getAnalysis<TargetLibraryAnalysis>(F);
  }

  /// Return AliasAnalysis Result for function \p F.
  AAResults *getAAResultsForFunction(const Function &F);

  /// Return true if \p Arg is involved in a must-tail call, thus the argument
  /// of the caller or callee.
  bool isInvolvedInMustTailCall(const Argument &Arg) {
    FunctionInfo &FI = getFunctionInfo(*Arg.getParent());
    return FI.CalledViaMustTail || FI.ContainsMustTailCall;
  }

  /// Return the analysis result from a pass \p AP for function \p F.
  template <typename AP>
  typename AP::Result *getAnalysisResultForFunction(const Function &F) {
    return AG.getAnalysis<AP>(F);
  }

  /// Return SCC size on call graph for function \p F or 0 if unknown.
  unsigned getSccSize(const Function &F) {
    if (CGSCC && CGSCC->count(const_cast<Function *>(&F)))
      return CGSCC->size();
    return 0;
  }

  /// Return datalayout used in the module.
  const DataLayout &getDL() { return DL; }

  /// Return the map conaining all the knowledge we have from `llvm.assume`s.
  const RetainedKnowledgeMap &getKnowledgeMap() const { return KnowledgeMap; }

  /// Return if \p To is potentially reachable form \p From or not
  /// If the same query was answered, return cached result
  bool getPotentiallyReachable(const Instruction &From, const Instruction &To) {
    auto KeyPair = std::make_pair(&From, &To);
    auto Iter = PotentiallyReachableMap.find(KeyPair);
    if (Iter != PotentiallyReachableMap.end())
      return Iter->second;
    const Function &F = *From.getFunction();
    bool Result = isPotentiallyReachable(
        &From, &To, nullptr, AG.getAnalysis<DominatorTreeAnalysis>(F),
        AG.getAnalysis<LoopAnalysis>(F));
    PotentiallyReachableMap.insert(std::make_pair(KeyPair, Result));
    return Result;
  }

  /// Check whether \p F is part of module slice.
  bool isInModuleSlice(const Function &F) {
    return ModuleSlice.count(const_cast<Function *>(&F));
  }

private:
  struct FunctionInfo {
    ~FunctionInfo();

    /// A nested map that remembers all instructions in a function with a
    /// certain instruction opcode (Instruction::getOpcode()).
    OpcodeInstMapTy OpcodeInstMap;

    /// A map from functions to their instructions that may read or write
    /// memory.
    InstructionVectorTy RWInsts;

    /// Function is called by a `musttail` call.
    bool CalledViaMustTail;

    /// Function contains a `musttail` call.
    bool ContainsMustTailCall;
  };

  /// A map type from functions to informatio about it.
  DenseMap<const Function *, FunctionInfo *> FuncInfoMap;

  /// Return information about the function \p F, potentially by creating it.
  FunctionInfo &getFunctionInfo(const Function &F) {
    FunctionInfo *&FI = FuncInfoMap[&F];
    if (!FI) {
      FI = new (Allocator) FunctionInfo();
      initializeInformationCache(F, *FI);
    }
    return *FI;
  }

  /// Initialize the function information cache \p FI for the function \p F.
  ///
  /// This method needs to be called for all function that might be looked at
  /// through the information cache interface *prior* to looking at them.
  void initializeInformationCache(const Function &F, FunctionInfo &FI);

  /// The datalayout used in the module.
  const DataLayout &DL;

  /// The allocator used to allocate memory, e.g. for `FunctionInfo`s.
  BumpPtrAllocator &Allocator;

  /// MustBeExecutedContextExplorer
  MustBeExecutedContextExplorer Explorer;

  /// A map with knowledge retained in `llvm.assume` instructions.
  RetainedKnowledgeMap KnowledgeMap;

  /// Getters for analysis.
  AnalysisGetter &AG;

  /// The underlying CGSCC, or null if not available.
  SetVector<Function *> *CGSCC;

  /// Set of inlineable functions
  SmallPtrSet<const Function *, 8> InlineableFunctions;

  /// A map for caching results of queries for isPotentiallyReachable
  DenseMap<std::pair<const Instruction *, const Instruction *>, bool>
      PotentiallyReachableMap;

  /// Give the Attributor access to the members so
  /// Attributor::identifyDefaultAbstractAttributes(...) can initialize them.
  friend struct Attributor;
};

/// The fixpoint analysis framework that orchestrates the attribute deduction.
///
/// The Attributor provides a general abstract analysis framework (guided
/// fixpoint iteration) as well as helper functions for the deduction of
/// (LLVM-IR) attributes. However, also other code properties can be deduced,
/// propagated, and ultimately manifested through the Attributor framework. This
/// is particularly useful if these properties interact with attributes and a
/// co-scheduled deduction allows to improve the solution. Even if not, thus if
/// attributes/properties are completely isolated, they should use the
/// Attributor framework to reduce the number of fixpoint iteration frameworks
/// in the code base. Note that the Attributor design makes sure that isolated
/// attributes are not impacted, in any way, by others derived at the same time
/// if there is no cross-reasoning performed.
///
/// The public facing interface of the Attributor is kept simple and basically
/// allows abstract attributes to one thing, query abstract attributes
/// in-flight. There are two reasons to do this:
///    a) The optimistic state of one abstract attribute can justify an
///       optimistic state of another, allowing to framework to end up with an
///       optimistic (=best possible) fixpoint instead of one based solely on
///       information in the IR.
///    b) This avoids reimplementing various kinds of lookups, e.g., to check
///       for existing IR attributes, in favor of a single lookups interface
///       provided by an abstract attribute subclass.
///
/// NOTE: The mechanics of adding a new "concrete" abstract attribute are
///       described in the file comment.
struct Attributor {
  /// Constructor
  ///
  /// \param Functions The set of functions we are deriving attributes for.
  /// \param InfoCache Cache to hold various information accessible for
  ///                  the abstract attributes.
  /// \param CGUpdater Helper to update an underlying call graph.
  /// \param Allowed If not null, a set limiting the attribute opportunities.
  /// \param DeleteFns Whether to delete functions
  Attributor(SetVector<Function *> &Functions, InformationCache &InfoCache,
             CallGraphUpdater &CGUpdater,
             DenseSet<const char *> *Allowed = nullptr, bool DeleteFns = true)
      : Allocator(InfoCache.Allocator), Functions(Functions),
        InfoCache(InfoCache), CGUpdater(CGUpdater), Allowed(Allowed),
        DeleteFns(DeleteFns) {}

  ~Attributor();

  /// Run the analyses until a fixpoint is reached or enforced (timeout).
  ///
  /// The attributes registered with this Attributor can be used after as long
  /// as the Attributor is not destroyed (it owns the attributes now).
  ///
  /// \Returns CHANGED if the IR was changed, otherwise UNCHANGED.
  ChangeStatus run();

  /// Lookup an abstract attribute of type \p AAType at position \p IRP. While
  /// no abstract attribute is found equivalent positions are checked, see
  /// SubsumingPositionIterator. Thus, the returned abstract attribute
  /// might be anchored at a different position, e.g., the callee if \p IRP is a
  /// call base.
  ///
  /// This method is the only (supported) way an abstract attribute can retrieve
  /// information from another abstract attribute. As an example, take an
  /// abstract attribute that determines the memory access behavior for a
  /// argument (readnone, readonly, ...). It should use `getAAFor` to get the
  /// most optimistic information for other abstract attributes in-flight, e.g.
  /// the one reasoning about the "captured" state for the argument or the one
  /// reasoning on the memory access behavior of the function as a whole.
  ///
  /// If the DepClass enum is set to `DepClassTy::None` the dependence from
  /// \p QueryingAA to the return abstract attribute is not automatically
  /// recorded. This should only be used if the caller will record the
  /// dependence explicitly if necessary, thus if it the returned abstract
  /// attribute is used for reasoning. To record the dependences explicitly use
  /// the `Attributor::recordDependence` method.
  template <typename AAType>
  const AAType &getAAFor(const AbstractAttribute &QueryingAA,
                         const IRPosition &IRP, DepClassTy DepClass) {
    return getOrCreateAAFor<AAType>(IRP, &QueryingAA, DepClass,
                                    /* ForceUpdate */ false);
  }

  /// Similar to getAAFor but the return abstract attribute will be updated (via
  /// `AbstractAttribute::update`) even if it is found in the cache. This is
  /// especially useful for AAIsDead as changes in liveness can make updates
  /// possible/useful that were not happening before as the abstract attribute
  /// was assumed dead.
  template <typename AAType>
  const AAType &getAndUpdateAAFor(const AbstractAttribute &QueryingAA,
                                  const IRPosition &IRP, DepClassTy DepClass) {
    return getOrCreateAAFor<AAType>(IRP, &QueryingAA, DepClass,
                                    /* ForceUpdate */ true);
  }

  /// The version of getAAFor that allows to omit a querying abstract
  /// attribute. Using this after Attributor started running is restricted to
  /// only the Attributor itself. Initial seeding of AAs can be done via this
  /// function.
  /// NOTE: ForceUpdate is ignored in any stage other than the update stage.
  template <typename AAType>
  const AAType &
  getOrCreateAAFor(IRPosition IRP, const AbstractAttribute *QueryingAA,
                   DepClassTy DepClass, bool ForceUpdate = false) {
#ifdef EXPENSIVE_CHECKS
    // Don't allow callbase information to leak.
    if (auto *CBContext = IRP.getCallBaseContext()) {
      assert(
          ((CBContext->getCalledFunction() == IRP.getAnchorScope() ||
            !QueryingAA ||
            !QueryingAA->getIRPosition().isAnyCallSitePosition())) &&
          "non callsite positions are not allowed to propagate CallBaseContext "
          "across functions");
    }
#endif
    if (!shouldPropagateCallBaseContext(IRP))
      IRP = IRP.stripCallBaseContext();

    if (AAType *AAPtr = lookupAAFor<AAType>(IRP, QueryingAA, DepClass)) {
      if (ForceUpdate && Phase == AttributorPhase::UPDATE)
        updateAA(*AAPtr);
      return *AAPtr;
    }

    // No matching attribute found, create one.
    // Use the static create method.
    auto &AA = AAType::createForPosition(IRP, *this);

    // If we are currenty seeding attributes, enforce seeding rules.
    if (Phase == AttributorPhase::SEEDING && !shouldSeedAttribute(AA)) {
      AA.getState().indicatePessimisticFixpoint();
      return AA;
    }

    registerAA(AA);

    // For now we ignore naked and optnone functions.
    bool Invalidate = Allowed && !Allowed->count(&AAType::ID);
    const Function *FnScope = IRP.getAnchorScope();
    if (FnScope)
      Invalidate |= FnScope->hasFnAttribute(Attribute::Naked) ||
                    FnScope->hasFnAttribute(Attribute::OptimizeNone);

    // Avoid too many nested initializations to prevent a stack overflow.
    Invalidate |= InitializationChainLength > MaxInitializationChainLength;

    // Bootstrap the new attribute with an initial update to propagate
    // information, e.g., function -> call site. If it is not on a given
    // Allowed we will not perform updates at all.
    if (Invalidate) {
      AA.getState().indicatePessimisticFixpoint();
      return AA;
    }

    {
      TimeTraceScope TimeScope(AA.getName() + "::initialize");
      ++InitializationChainLength;
      AA.initialize(*this);
      --InitializationChainLength;
    }

    // Initialize and update is allowed for code outside of the current function
    // set, but only if it is part of module slice we are allowed to look at.
    // Only exception is AAIsDeadFunction whose initialization is prevented
    // directly, since we don't to compute it twice.
    if (FnScope && !Functions.count(const_cast<Function *>(FnScope))) {
      if (!getInfoCache().isInModuleSlice(*FnScope)) {
        AA.getState().indicatePessimisticFixpoint();
        return AA;
      }
    }

    // If this is queried in the manifest stage, we force the AA to indicate
    // pessimistic fixpoint immediately.
    if (Phase == AttributorPhase::MANIFEST) {
      AA.getState().indicatePessimisticFixpoint();
      return AA;
    }

    // Allow seeded attributes to declare dependencies.
    // Remember the seeding state.
    AttributorPhase OldPhase = Phase;
    Phase = AttributorPhase::UPDATE;

    updateAA(AA);

    Phase = OldPhase;

    if (QueryingAA && AA.getState().isValidState())
      recordDependence(AA, const_cast<AbstractAttribute &>(*QueryingAA),
                       DepClass);
    return AA;
  }
  template <typename AAType>
  const AAType &getOrCreateAAFor(const IRPosition &IRP) {
    return getOrCreateAAFor<AAType>(IRP, /* QueryingAA */ nullptr,
                                    DepClassTy::NONE);
  }

  /// Return the attribute of \p AAType for \p IRP if existing. This also allows
  /// non-AA users lookup.
  template <typename AAType>
  AAType *lookupAAFor(const IRPosition &IRP,
                      const AbstractAttribute *QueryingAA = nullptr,
                      DepClassTy DepClass = DepClassTy::OPTIONAL) {
    static_assert(std::is_base_of<AbstractAttribute, AAType>::value,
                  "Cannot query an attribute with a type not derived from "
                  "'AbstractAttribute'!");
    // Lookup the abstract attribute of type AAType. If found, return it after
    // registering a dependence of QueryingAA on the one returned attribute.
    AbstractAttribute *AAPtr = AAMap.lookup({&AAType::ID, IRP});
    if (!AAPtr)
      return nullptr;

    AAType *AA = static_cast<AAType *>(AAPtr);

    // Do not register a dependence on an attribute with an invalid state.
    if (DepClass != DepClassTy::NONE && QueryingAA &&
        AA->getState().isValidState())
      recordDependence(*AA, const_cast<AbstractAttribute &>(*QueryingAA),
                       DepClass);
    return AA;
  }

  /// Explicitly record a dependence from \p FromAA to \p ToAA, that is if
  /// \p FromAA changes \p ToAA should be updated as well.
  ///
  /// This method should be used in conjunction with the `getAAFor` method and
  /// with the DepClass enum passed to the method set to None. This can
  /// be beneficial to avoid false dependences but it requires the users of
  /// `getAAFor` to explicitly record true dependences through this method.
  /// The \p DepClass flag indicates if the dependence is striclty necessary.
  /// That means for required dependences, if \p FromAA changes to an invalid
  /// state, \p ToAA can be moved to a pessimistic fixpoint because it required
  /// information from \p FromAA but none are available anymore.
  void recordDependence(const AbstractAttribute &FromAA,
                        const AbstractAttribute &ToAA, DepClassTy DepClass);

  /// Introduce a new abstract attribute into the fixpoint analysis.
  ///
  /// Note that ownership of the attribute is given to the Attributor. It will
  /// invoke delete for the Attributor on destruction of the Attributor.
  ///
  /// Attributes are identified by their IR position (AAType::getIRPosition())
  /// and the address of their static member (see AAType::ID).
  template <typename AAType> AAType &registerAA(AAType &AA) {
    static_assert(std::is_base_of<AbstractAttribute, AAType>::value,
                  "Cannot register an attribute with a type not derived from "
                  "'AbstractAttribute'!");
    // Put the attribute in the lookup map structure and the container we use to
    // keep track of all attributes.
    const IRPosition &IRP = AA.getIRPosition();
    AbstractAttribute *&AAPtr = AAMap[{&AAType::ID, IRP}];

    assert(!AAPtr && "Attribute already in map!");
    AAPtr = &AA;

    // Register AA with the synthetic root only before the manifest stage.
    if (Phase == AttributorPhase::SEEDING || Phase == AttributorPhase::UPDATE)
      DG.SyntheticRoot.Deps.push_back(
          AADepGraphNode::DepTy(&AA, unsigned(DepClassTy::REQUIRED)));

    return AA;
  }

  /// Return the internal information cache.
  InformationCache &getInfoCache() { return InfoCache; }

  /// Return true if this is a module pass, false otherwise.
  bool isModulePass() const {
    return !Functions.empty() &&
           Functions.size() == Functions.front()->getParent()->size();
  }

  /// Return true if we derive attributes for \p Fn
  bool isRunOn(Function &Fn) const {
    return Functions.empty() || Functions.count(&Fn);
  }

  /// Determine opportunities to derive 'default' attributes in \p F and create
  /// abstract attribute objects for them.
  ///
  /// \param F The function that is checked for attribute opportunities.
  ///
  /// Note that abstract attribute instances are generally created even if the
  /// IR already contains the information they would deduce. The most important
  /// reason for this is the single interface, the one of the abstract attribute
  /// instance, which can be queried without the need to look at the IR in
  /// various places.
  void identifyDefaultAbstractAttributes(Function &F);

  /// Determine whether the function \p F is IPO amendable
  ///
  /// If a function is exactly defined or it has alwaysinline attribute
  /// and is viable to be inlined, we say it is IPO amendable
  bool isFunctionIPOAmendable(const Function &F) {
    return F.hasExactDefinition() || InfoCache.InlineableFunctions.count(&F);
  }

  /// Mark the internal function \p F as live.
  ///
  /// This will trigger the identification and initialization of attributes for
  /// \p F.
  void markLiveInternalFunction(const Function &F) {
    assert(F.hasLocalLinkage() &&
           "Only local linkage is assumed dead initially.");

    identifyDefaultAbstractAttributes(const_cast<Function &>(F));
  }

  /// Helper function to remove callsite.
  void removeCallSite(CallInst *CI) {
    if (!CI)
      return;

    CGUpdater.removeCallSite(*CI);
  }

  /// Record that \p U is to be replaces with \p NV after information was
  /// manifested. This also triggers deletion of trivially dead istructions.
  bool changeUseAfterManifest(Use &U, Value &NV) {
    Value *&V = ToBeChangedUses[&U];
    if (V && (V->stripPointerCasts() == NV.stripPointerCasts() ||
              isa_and_nonnull<UndefValue>(V)))
      return false;
    assert((!V || V == &NV || isa<UndefValue>(NV)) &&
           "Use was registered twice for replacement with different values!");
    V = &NV;
    return true;
  }

  /// Helper function to replace all uses of \p V with \p NV. Return true if
  /// there is any change. The flag \p ChangeDroppable indicates if dropppable
  /// uses should be changed too.
  bool changeValueAfterManifest(Value &V, Value &NV,
                                bool ChangeDroppable = true) {
    bool Changed = false;
    for (auto &U : V.uses())
      if (ChangeDroppable || !U.getUser()->isDroppable())
        Changed |= changeUseAfterManifest(U, NV);

    return Changed;
  }

  /// Record that \p I is to be replaced with `unreachable` after information
  /// was manifested.
  void changeToUnreachableAfterManifest(Instruction *I) {
    ToBeChangedToUnreachableInsts.insert(I);
  }

  /// Record that \p II has at least one dead successor block. This information
  /// is used, e.g., to replace \p II with a call, after information was
  /// manifested.
  void registerInvokeWithDeadSuccessor(InvokeInst &II) {
    InvokeWithDeadSuccessor.push_back(&II);
  }

  /// Record that \p I is deleted after information was manifested. This also
  /// triggers deletion of trivially dead istructions.
  void deleteAfterManifest(Instruction &I) { ToBeDeletedInsts.insert(&I); }

  /// Record that \p BB is deleted after information was manifested. This also
  /// triggers deletion of trivially dead istructions.
  void deleteAfterManifest(BasicBlock &BB) { ToBeDeletedBlocks.insert(&BB); }

  /// Record that \p F is deleted after information was manifested.
  void deleteAfterManifest(Function &F) {
    if (DeleteFns)
      ToBeDeletedFunctions.insert(&F);
  }

  /// If \p V is assumed to be a constant, return it, if it is unclear yet,
  /// return None, otherwise return `nullptr`.
  Optional<Constant *> getAssumedConstant(const Value &V,
                                          const AbstractAttribute &AA,
                                          bool &UsedAssumedInformation);

  /// Return true if \p AA (or its context instruction) is assumed dead.
  ///
  /// If \p LivenessAA is not provided it is queried.
  bool isAssumedDead(const AbstractAttribute &AA, const AAIsDead *LivenessAA,
                     bool CheckBBLivenessOnly = false,
                     DepClassTy DepClass = DepClassTy::OPTIONAL);

  /// Return true if \p I is assumed dead.
  ///
  /// If \p LivenessAA is not provided it is queried.
  bool isAssumedDead(const Instruction &I, const AbstractAttribute *QueryingAA,
                     const AAIsDead *LivenessAA,
                     bool CheckBBLivenessOnly = false,
                     DepClassTy DepClass = DepClassTy::OPTIONAL);

  /// Return true if \p U is assumed dead.
  ///
  /// If \p FnLivenessAA is not provided it is queried.
  bool isAssumedDead(const Use &U, const AbstractAttribute *QueryingAA,
                     const AAIsDead *FnLivenessAA,
                     bool CheckBBLivenessOnly = false,
                     DepClassTy DepClass = DepClassTy::OPTIONAL);

  /// Return true if \p IRP is assumed dead.
  ///
  /// If \p FnLivenessAA is not provided it is queried.
  bool isAssumedDead(const IRPosition &IRP, const AbstractAttribute *QueryingAA,
                     const AAIsDead *FnLivenessAA,
                     bool CheckBBLivenessOnly = false,
                     DepClassTy DepClass = DepClassTy::OPTIONAL);

  /// Check \p Pred on all (transitive) uses of \p V.
  ///
  /// This method will evaluate \p Pred on all (transitive) uses of the
  /// associated value and return true if \p Pred holds every time.
  bool checkForAllUses(function_ref<bool(const Use &, bool &)> Pred,
                       const AbstractAttribute &QueryingAA, const Value &V,
                       DepClassTy LivenessDepClass = DepClassTy::OPTIONAL);

  /// Helper struct used in the communication between an abstract attribute (AA)
  /// that wants to change the signature of a function and the Attributor which
  /// applies the changes. The struct is partially initialized with the
  /// information from the AA (see the constructor). All other members are
  /// provided by the Attributor prior to invoking any callbacks.
  struct ArgumentReplacementInfo {
    /// Callee repair callback type
    ///
    /// The function repair callback is invoked once to rewire the replacement
    /// arguments in the body of the new function. The argument replacement info
    /// is passed, as build from the registerFunctionSignatureRewrite call, as
    /// well as the replacement function and an iteratore to the first
    /// replacement argument.
    using CalleeRepairCBTy = std::function<void(
        const ArgumentReplacementInfo &, Function &, Function::arg_iterator)>;

    /// Abstract call site (ACS) repair callback type
    ///
    /// The abstract call site repair callback is invoked once on every abstract
    /// call site of the replaced function (\see ReplacedFn). The callback needs
    /// to provide the operands for the call to the new replacement function.
    /// The number and type of the operands appended to the provided vector
    /// (second argument) is defined by the number and types determined through
    /// the replacement type vector (\see ReplacementTypes). The first argument
    /// is the ArgumentReplacementInfo object registered with the Attributor
    /// through the registerFunctionSignatureRewrite call.
    using ACSRepairCBTy =
        std::function<void(const ArgumentReplacementInfo &, AbstractCallSite,
                           SmallVectorImpl<Value *> &)>;

    /// Simple getters, see the corresponding members for details.
    ///{

    Attributor &getAttributor() const { return A; }
    const Function &getReplacedFn() const { return ReplacedFn; }
    const Argument &getReplacedArg() const { return ReplacedArg; }
    unsigned getNumReplacementArgs() const { return ReplacementTypes.size(); }
    const SmallVectorImpl<Type *> &getReplacementTypes() const {
      return ReplacementTypes;
    }

    ///}

  private:
    /// Constructor that takes the argument to be replaced, the types of
    /// the replacement arguments, as well as callbacks to repair the call sites
    /// and new function after the replacement happened.
    ArgumentReplacementInfo(Attributor &A, Argument &Arg,
                            ArrayRef<Type *> ReplacementTypes,
                            CalleeRepairCBTy &&CalleeRepairCB,
                            ACSRepairCBTy &&ACSRepairCB)
        : A(A), ReplacedFn(*Arg.getParent()), ReplacedArg(Arg),
          ReplacementTypes(ReplacementTypes.begin(), ReplacementTypes.end()),
          CalleeRepairCB(std::move(CalleeRepairCB)),
          ACSRepairCB(std::move(ACSRepairCB)) {}

    /// Reference to the attributor to allow access from the callbacks.
    Attributor &A;

    /// The "old" function replaced by ReplacementFn.
    const Function &ReplacedFn;

    /// The "old" argument replaced by new ones defined via ReplacementTypes.
    const Argument &ReplacedArg;

    /// The types of the arguments replacing ReplacedArg.
    const SmallVector<Type *, 8> ReplacementTypes;

    /// Callee repair callback, see CalleeRepairCBTy.
    const CalleeRepairCBTy CalleeRepairCB;

    /// Abstract call site (ACS) repair callback, see ACSRepairCBTy.
    const ACSRepairCBTy ACSRepairCB;

    /// Allow access to the private members from the Attributor.
    friend struct Attributor;
  };

  /// Check if we can rewrite a function signature.
  ///
  /// The argument \p Arg is replaced with new ones defined by the number,
  /// order, and types in \p ReplacementTypes.
  ///
  /// \returns True, if the replacement can be registered, via
  /// registerFunctionSignatureRewrite, false otherwise.
  bool isValidFunctionSignatureRewrite(Argument &Arg,
                                       ArrayRef<Type *> ReplacementTypes);

  /// Register a rewrite for a function signature.
  ///
  /// The argument \p Arg is replaced with new ones defined by the number,
  /// order, and types in \p ReplacementTypes. The rewiring at the call sites is
  /// done through \p ACSRepairCB and at the callee site through
  /// \p CalleeRepairCB.
  ///
  /// \returns True, if the replacement was registered, false otherwise.
  bool registerFunctionSignatureRewrite(
      Argument &Arg, ArrayRef<Type *> ReplacementTypes,
      ArgumentReplacementInfo::CalleeRepairCBTy &&CalleeRepairCB,
      ArgumentReplacementInfo::ACSRepairCBTy &&ACSRepairCB);

  /// Check \p Pred on all function call sites.
  ///
  /// This method will evaluate \p Pred on call sites and return
  /// true if \p Pred holds in every call sites. However, this is only possible
  /// all call sites are known, hence the function has internal linkage.
  /// If true is returned, \p AllCallSitesKnown is set if all possible call
  /// sites of the function have been visited.
  bool checkForAllCallSites(function_ref<bool(AbstractCallSite)> Pred,
                            const AbstractAttribute &QueryingAA,
                            bool RequireAllCallSites, bool &AllCallSitesKnown);

  /// Check \p Pred on all values potentially returned by \p F.
  ///
  /// This method will evaluate \p Pred on all values potentially returned by
  /// the function associated with \p QueryingAA. The returned values are
  /// matched with their respective return instructions. Returns true if \p Pred
  /// holds on all of them.
  bool checkForAllReturnedValuesAndReturnInsts(
      function_ref<bool(Value &, const SmallSetVector<ReturnInst *, 4> &)> Pred,
      const AbstractAttribute &QueryingAA);

  /// Check \p Pred on all values potentially returned by the function
  /// associated with \p QueryingAA.
  ///
  /// This is the context insensitive version of the method above.
  bool checkForAllReturnedValues(function_ref<bool(Value &)> Pred,
                                 const AbstractAttribute &QueryingAA);

  /// Check \p Pred on all instructions with an opcode present in \p Opcodes.
  ///
  /// This method will evaluate \p Pred on all instructions with an opcode
  /// present in \p Opcode and return true if \p Pred holds on all of them.
  bool checkForAllInstructions(function_ref<bool(Instruction &)> Pred,
                               const AbstractAttribute &QueryingAA,
                               const ArrayRef<unsigned> &Opcodes,
                               bool CheckBBLivenessOnly = false);

  /// Check \p Pred on all call-like instructions (=CallBased derived).
  ///
  /// See checkForAllCallLikeInstructions(...) for more information.
  bool checkForAllCallLikeInstructions(function_ref<bool(Instruction &)> Pred,
                                       const AbstractAttribute &QueryingAA) {
    return checkForAllInstructions(Pred, QueryingAA,
                                   {(unsigned)Instruction::Invoke,
                                    (unsigned)Instruction::CallBr,
                                    (unsigned)Instruction::Call});
  }

  /// Check \p Pred on all Read/Write instructions.
  ///
  /// This method will evaluate \p Pred on all instructions that read or write
  /// to memory present in the information cache and return true if \p Pred
  /// holds on all of them.
  bool checkForAllReadWriteInstructions(function_ref<bool(Instruction &)> Pred,
                                        AbstractAttribute &QueryingAA);

  /// Create a shallow wrapper for \p F such that \p F has internal linkage
  /// afterwards. It also sets the original \p F 's name to anonymous
  ///
  /// A wrapper is a function with the same type (and attributes) as \p F
  /// that will only call \p F and return the result, if any.
  ///
  /// Assuming the declaration of looks like:
  ///   rty F(aty0 arg0, ..., atyN argN);
  ///
  /// The wrapper will then look as follows:
  ///   rty wrapper(aty0 arg0, ..., atyN argN) {
  ///     return F(arg0, ..., argN);
  ///   }
  ///
  static void createShallowWrapper(Function &F);

  /// Return the data layout associated with the anchor scope.
  const DataLayout &getDataLayout() const { return InfoCache.DL; }

  /// The allocator used to allocate memory, e.g. for `AbstractAttribute`s.
  BumpPtrAllocator &Allocator;

private:
  /// This method will do fixpoint iteration until fixpoint or the
  /// maximum iteration count is reached.
  ///
  /// If the maximum iteration count is reached, This method will
  /// indicate pessimistic fixpoint on attributes that transitively depend
  /// on attributes that were scheduled for an update.
  void runTillFixpoint();

  /// Gets called after scheduling, manifests attributes to the LLVM IR.
  ChangeStatus manifestAttributes();

  /// Gets called after attributes have been manifested, cleans up the IR.
  /// Deletes dead functions, blocks and instructions.
  /// Rewrites function signitures and updates the call graph.
  ChangeStatus cleanupIR();

  /// Identify internal functions that are effectively dead, thus not reachable
  /// from a live entry point. The functions are added to ToBeDeletedFunctions.
  void identifyDeadInternalFunctions();

  /// Run `::update` on \p AA and track the dependences queried while doing so.
  /// Also adjust the state if we know further updates are not necessary.
  ChangeStatus updateAA(AbstractAttribute &AA);

  /// Remember the dependences on the top of the dependence stack such that they
  /// may trigger further updates. (\see DependenceStack)
  void rememberDependences();

  /// Check \p Pred on all call sites of \p Fn.
  ///
  /// This method will evaluate \p Pred on call sites and return
  /// true if \p Pred holds in every call sites. However, this is only possible
  /// all call sites are known, hence the function has internal linkage.
  /// If true is returned, \p AllCallSitesKnown is set if all possible call
  /// sites of the function have been visited.
  bool checkForAllCallSites(function_ref<bool(AbstractCallSite)> Pred,
                            const Function &Fn, bool RequireAllCallSites,
                            const AbstractAttribute *QueryingAA,
                            bool &AllCallSitesKnown);

  /// Determine if CallBase context in \p IRP should be propagated.
  bool shouldPropagateCallBaseContext(const IRPosition &IRP);

  /// Apply all requested function signature rewrites
  /// (\see registerFunctionSignatureRewrite) and return Changed if the module
  /// was altered.
  ChangeStatus
  rewriteFunctionSignatures(SmallPtrSetImpl<Function *> &ModifiedFns);

  /// Check if the Attribute \p AA should be seeded.
  /// See getOrCreateAAFor.
  bool shouldSeedAttribute(AbstractAttribute &AA);

  /// A nested map to lookup abstract attributes based on the argument position
  /// on the outer level, and the addresses of the static member (AAType::ID) on
  /// the inner level.
  ///{
  using AAMapKeyTy = std::pair<const char *, IRPosition>;
  DenseMap<AAMapKeyTy, AbstractAttribute *> AAMap;
  ///}

  /// Map to remember all requested signature changes (= argument replacements).
  DenseMap<Function *, SmallVector<std::unique_ptr<ArgumentReplacementInfo>, 8>>
      ArgumentReplacementMap;

  /// The set of functions we are deriving attributes for.
  SetVector<Function *> &Functions;

  /// The information cache that holds pre-processed (LLVM-IR) information.
  InformationCache &InfoCache;

  /// Helper to update an underlying call graph.
  CallGraphUpdater &CGUpdater;

  /// Abstract Attribute dependency graph
  AADepGraph DG;

  /// Set of functions for which we modified the content such that it might
  /// impact the call graph.
  SmallPtrSet<Function *, 8> CGModifiedFunctions;

  /// Information about a dependence. If FromAA is changed ToAA needs to be
  /// updated as well.
  struct DepInfo {
    const AbstractAttribute *FromAA;
    const AbstractAttribute *ToAA;
    DepClassTy DepClass;
  };

  /// The dependence stack is used to track dependences during an
  /// `AbstractAttribute::update` call. As `AbstractAttribute::update` can be
  /// recursive we might have multiple vectors of dependences in here. The stack
  /// size, should be adjusted according to the expected recursion depth and the
  /// inner dependence vector size to the expected number of dependences per
  /// abstract attribute. Since the inner vectors are actually allocated on the
  /// stack we can be generous with their size.
  using DependenceVector = SmallVector<DepInfo, 8>;
  SmallVector<DependenceVector *, 16> DependenceStack;

  /// If not null, a set limiting the attribute opportunities.
  const DenseSet<const char *> *Allowed;

  /// Whether to delete functions.
  const bool DeleteFns;

  /// A set to remember the functions we already assume to be live and visited.
  DenseSet<const Function *> VisitedFunctions;

  /// Uses we replace with a new value after manifest is done. We will remove
  /// then trivially dead instructions as well.
  DenseMap<Use *, Value *> ToBeChangedUses;

  /// Instructions we replace with `unreachable` insts after manifest is done.
  SmallDenseSet<WeakVH, 16> ToBeChangedToUnreachableInsts;

  /// Invoke instructions with at least a single dead successor block.
  SmallVector<WeakVH, 16> InvokeWithDeadSuccessor;

  /// A flag that indicates which stage of the process we are in. Initially, the
  /// phase is SEEDING. Phase is changed in `Attributor::run()`
  enum class AttributorPhase {
    SEEDING,
    UPDATE,
    MANIFEST,
    CLEANUP,
  } Phase = AttributorPhase::SEEDING;

  /// The current initialization chain length. Tracked to avoid stack overflows.
  unsigned InitializationChainLength = 0;

  /// Functions, blocks, and instructions we delete after manifest is done.
  ///
  ///{
  SmallPtrSet<Function *, 8> ToBeDeletedFunctions;
  SmallPtrSet<BasicBlock *, 8> ToBeDeletedBlocks;
  SmallDenseSet<WeakVH, 8> ToBeDeletedInsts;
  ///}

  friend AADepGraph;
};

/// An interface to query the internal state of an abstract attribute.
///
/// The abstract state is a minimal interface that allows the Attributor to
/// communicate with the abstract attributes about their internal state without
/// enforcing or exposing implementation details, e.g., the (existence of an)
/// underlying lattice.
///
/// It is sufficient to be able to query if a state is (1) valid or invalid, (2)
/// at a fixpoint, and to indicate to the state that (3) an optimistic fixpoint
/// was reached or (4) a pessimistic fixpoint was enforced.
///
/// All methods need to be implemented by the subclass. For the common use case,
/// a single boolean state or a bit-encoded state, the BooleanState and
/// {Inc,Dec,Bit}IntegerState classes are already provided. An abstract
/// attribute can inherit from them to get the abstract state interface and
/// additional methods to directly modify the state based if needed. See the
/// class comments for help.
struct AbstractState {
  virtual ~AbstractState() {}

  /// Return if this abstract state is in a valid state. If false, no
  /// information provided should be used.
  virtual bool isValidState() const = 0;

  /// Return if this abstract state is fixed, thus does not need to be updated
  /// if information changes as it cannot change itself.
  virtual bool isAtFixpoint() const = 0;

  /// Indicate that the abstract state should converge to the optimistic state.
  ///
  /// This will usually make the optimistically assumed state the known to be
  /// true state.
  ///
  /// \returns ChangeStatus::UNCHANGED as the assumed value should not change.
  virtual ChangeStatus indicateOptimisticFixpoint() = 0;

  /// Indicate that the abstract state should converge to the pessimistic state.
  ///
  /// This will usually revert the optimistically assumed state to the known to
  /// be true state.
  ///
  /// \returns ChangeStatus::CHANGED as the assumed value may change.
  virtual ChangeStatus indicatePessimisticFixpoint() = 0;
};

/// Simple state with integers encoding.
///
/// The interface ensures that the assumed bits are always a subset of the known
/// bits. Users can only add known bits and, except through adding known bits,
/// they can only remove assumed bits. This should guarantee monotoniticy and
/// thereby the existence of a fixpoint (if used corretly). The fixpoint is
/// reached when the assumed and known state/bits are equal. Users can
/// force/inidicate a fixpoint. If an optimistic one is indicated, the known
/// state will catch up with the assumed one, for a pessimistic fixpoint it is
/// the other way around.
template <typename base_ty, base_ty BestState, base_ty WorstState>
struct IntegerStateBase : public AbstractState {
  using base_t = base_ty;

  IntegerStateBase() {}
  IntegerStateBase(base_t Assumed) : Assumed(Assumed) {}

  /// Return the best possible representable state.
  static constexpr base_t getBestState() { return BestState; }
  static constexpr base_t getBestState(const IntegerStateBase &) {
    return getBestState();
  }

  /// Return the worst possible representable state.
  static constexpr base_t getWorstState() { return WorstState; }
  static constexpr base_t getWorstState(const IntegerStateBase &) {
    return getWorstState();
  }

  /// See AbstractState::isValidState()
  /// NOTE: For now we simply pretend that the worst possible state is invalid.
  bool isValidState() const override { return Assumed != getWorstState(); }

  /// See AbstractState::isAtFixpoint()
  bool isAtFixpoint() const override { return Assumed == Known; }

  /// See AbstractState::indicateOptimisticFixpoint(...)
  ChangeStatus indicateOptimisticFixpoint() override {
    Known = Assumed;
    return ChangeStatus::UNCHANGED;
  }

  /// See AbstractState::indicatePessimisticFixpoint(...)
  ChangeStatus indicatePessimisticFixpoint() override {
    Assumed = Known;
    return ChangeStatus::CHANGED;
  }

  /// Return the known state encoding
  base_t getKnown() const { return Known; }

  /// Return the assumed state encoding.
  base_t getAssumed() const { return Assumed; }

  /// Equality for IntegerStateBase.
  bool
  operator==(const IntegerStateBase<base_t, BestState, WorstState> &R) const {
    return this->getAssumed() == R.getAssumed() &&
           this->getKnown() == R.getKnown();
  }

  /// Inequality for IntegerStateBase.
  bool
  operator!=(const IntegerStateBase<base_t, BestState, WorstState> &R) const {
    return !(*this == R);
  }

  /// "Clamp" this state with \p R. The result is subtype dependent but it is
  /// intended that only information assumed in both states will be assumed in
  /// this one afterwards.
  void operator^=(const IntegerStateBase<base_t, BestState, WorstState> &R) {
    handleNewAssumedValue(R.getAssumed());
  }

  /// "Clamp" this state with \p R. The result is subtype dependent but it is
  /// intended that information known in either state will be known in
  /// this one afterwards.
  void operator+=(const IntegerStateBase<base_t, BestState, WorstState> &R) {
    handleNewKnownValue(R.getKnown());
  }

  void operator|=(const IntegerStateBase<base_t, BestState, WorstState> &R) {
    joinOR(R.getAssumed(), R.getKnown());
  }

  void operator&=(const IntegerStateBase<base_t, BestState, WorstState> &R) {
    joinAND(R.getAssumed(), R.getKnown());
  }

protected:
  /// Handle a new assumed value \p Value. Subtype dependent.
  virtual void handleNewAssumedValue(base_t Value) = 0;

  /// Handle a new known value \p Value. Subtype dependent.
  virtual void handleNewKnownValue(base_t Value) = 0;

  /// Handle a  value \p Value. Subtype dependent.
  virtual void joinOR(base_t AssumedValue, base_t KnownValue) = 0;

  /// Handle a new assumed value \p Value. Subtype dependent.
  virtual void joinAND(base_t AssumedValue, base_t KnownValue) = 0;

  /// The known state encoding in an integer of type base_t.
  base_t Known = getWorstState();

  /// The assumed state encoding in an integer of type base_t.
  base_t Assumed = getBestState();
};

/// Specialization of the integer state for a bit-wise encoding.
template <typename base_ty = uint32_t, base_ty BestState = ~base_ty(0),
          base_ty WorstState = 0>
struct BitIntegerState
    : public IntegerStateBase<base_ty, BestState, WorstState> {
  using base_t = base_ty;

  /// Return true if the bits set in \p BitsEncoding are "known bits".
  bool isKnown(base_t BitsEncoding) const {
    return (this->Known & BitsEncoding) == BitsEncoding;
  }

  /// Return true if the bits set in \p BitsEncoding are "assumed bits".
  bool isAssumed(base_t BitsEncoding) const {
    return (this->Assumed & BitsEncoding) == BitsEncoding;
  }

  /// Add the bits in \p BitsEncoding to the "known bits".
  BitIntegerState &addKnownBits(base_t Bits) {
    // Make sure we never miss any "known bits".
    this->Assumed |= Bits;
    this->Known |= Bits;
    return *this;
  }

  /// Remove the bits in \p BitsEncoding from the "assumed bits" if not known.
  BitIntegerState &removeAssumedBits(base_t BitsEncoding) {
    return intersectAssumedBits(~BitsEncoding);
  }

  /// Remove the bits in \p BitsEncoding from the "known bits".
  BitIntegerState &removeKnownBits(base_t BitsEncoding) {
    this->Known = (this->Known & ~BitsEncoding);
    return *this;
  }

  /// Keep only "assumed bits" also set in \p BitsEncoding but all known ones.
  BitIntegerState &intersectAssumedBits(base_t BitsEncoding) {
    // Make sure we never loose any "known bits".
    this->Assumed = (this->Assumed & BitsEncoding) | this->Known;
    return *this;
  }

private:
  void handleNewAssumedValue(base_t Value) override {
    intersectAssumedBits(Value);
  }
  void handleNewKnownValue(base_t Value) override { addKnownBits(Value); }
  void joinOR(base_t AssumedValue, base_t KnownValue) override {
    this->Known |= KnownValue;
    this->Assumed |= AssumedValue;
  }
  void joinAND(base_t AssumedValue, base_t KnownValue) override {
    this->Known &= KnownValue;
    this->Assumed &= AssumedValue;
  }
};

/// Specialization of the integer state for an increasing value, hence ~0u is
/// the best state and 0 the worst.
template <typename base_ty = uint32_t, base_ty BestState = ~base_ty(0),
          base_ty WorstState = 0>
struct IncIntegerState
    : public IntegerStateBase<base_ty, BestState, WorstState> {
  using super = IntegerStateBase<base_ty, BestState, WorstState>;
  using base_t = base_ty;

  IncIntegerState() : super() {}
  IncIntegerState(base_t Assumed) : super(Assumed) {}

  /// Return the best possible representable state.
  static constexpr base_t getBestState() { return BestState; }
  static constexpr base_t
  getBestState(const IncIntegerState<base_ty, BestState, WorstState> &) {
    return getBestState();
  }

  /// Take minimum of assumed and \p Value.
  IncIntegerState &takeAssumedMinimum(base_t Value) {
    // Make sure we never loose "known value".
    this->Assumed = std::max(std::min(this->Assumed, Value), this->Known);
    return *this;
  }

  /// Take maximum of known and \p Value.
  IncIntegerState &takeKnownMaximum(base_t Value) {
    // Make sure we never loose "known value".
    this->Assumed = std::max(Value, this->Assumed);
    this->Known = std::max(Value, this->Known);
    return *this;
  }

private:
  void handleNewAssumedValue(base_t Value) override {
    takeAssumedMinimum(Value);
  }
  void handleNewKnownValue(base_t Value) override { takeKnownMaximum(Value); }
  void joinOR(base_t AssumedValue, base_t KnownValue) override {
    this->Known = std::max(this->Known, KnownValue);
    this->Assumed = std::max(this->Assumed, AssumedValue);
  }
  void joinAND(base_t AssumedValue, base_t KnownValue) override {
    this->Known = std::min(this->Known, KnownValue);
    this->Assumed = std::min(this->Assumed, AssumedValue);
  }
};

/// Specialization of the integer state for a decreasing value, hence 0 is the
/// best state and ~0u the worst.
template <typename base_ty = uint32_t>
struct DecIntegerState : public IntegerStateBase<base_ty, 0, ~base_ty(0)> {
  using base_t = base_ty;

  /// Take maximum of assumed and \p Value.
  DecIntegerState &takeAssumedMaximum(base_t Value) {
    // Make sure we never loose "known value".
    this->Assumed = std::min(std::max(this->Assumed, Value), this->Known);
    return *this;
  }

  /// Take minimum of known and \p Value.
  DecIntegerState &takeKnownMinimum(base_t Value) {
    // Make sure we never loose "known value".
    this->Assumed = std::min(Value, this->Assumed);
    this->Known = std::min(Value, this->Known);
    return *this;
  }

private:
  void handleNewAssumedValue(base_t Value) override {
    takeAssumedMaximum(Value);
  }
  void handleNewKnownValue(base_t Value) override { takeKnownMinimum(Value); }
  void joinOR(base_t AssumedValue, base_t KnownValue) override {
    this->Assumed = std::min(this->Assumed, KnownValue);
    this->Assumed = std::min(this->Assumed, AssumedValue);
  }
  void joinAND(base_t AssumedValue, base_t KnownValue) override {
    this->Assumed = std::max(this->Assumed, KnownValue);
    this->Assumed = std::max(this->Assumed, AssumedValue);
  }
};

/// Simple wrapper for a single bit (boolean) state.
struct BooleanState : public IntegerStateBase<bool, 1, 0> {
  using super = IntegerStateBase<bool, 1, 0>;
  using base_t = IntegerStateBase::base_t;

  BooleanState() : super() {}
  BooleanState(base_t Assumed) : super(Assumed) {}

  /// Set the assumed value to \p Value but never below the known one.
  void setAssumed(bool Value) { Assumed &= (Known | Value); }

  /// Set the known and asssumed value to \p Value.
  void setKnown(bool Value) {
    Known |= Value;
    Assumed |= Value;
  }

  /// Return true if the state is assumed to hold.
  bool isAssumed() const { return getAssumed(); }

  /// Return true if the state is known to hold.
  bool isKnown() const { return getKnown(); }

private:
  void handleNewAssumedValue(base_t Value) override {
    if (!Value)
      Assumed = Known;
  }
  void handleNewKnownValue(base_t Value) override {
    if (Value)
      Known = (Assumed = Value);
  }
  void joinOR(base_t AssumedValue, base_t KnownValue) override {
    Known |= KnownValue;
    Assumed |= AssumedValue;
  }
  void joinAND(base_t AssumedValue, base_t KnownValue) override {
    Known &= KnownValue;
    Assumed &= AssumedValue;
  }
};

/// State for an integer range.
struct IntegerRangeState : public AbstractState {

  /// Bitwidth of the associated value.
  uint32_t BitWidth;

  /// State representing assumed range, initially set to empty.
  ConstantRange Assumed;

  /// State representing known range, initially set to [-inf, inf].
  ConstantRange Known;

  IntegerRangeState(uint32_t BitWidth)
      : BitWidth(BitWidth), Assumed(ConstantRange::getEmpty(BitWidth)),
        Known(ConstantRange::getFull(BitWidth)) {}

  IntegerRangeState(const ConstantRange &CR)
      : BitWidth(CR.getBitWidth()), Assumed(CR),
        Known(getWorstState(CR.getBitWidth())) {}

  /// Return the worst possible representable state.
  static ConstantRange getWorstState(uint32_t BitWidth) {
    return ConstantRange::getFull(BitWidth);
  }

  /// Return the best possible representable state.
  static ConstantRange getBestState(uint32_t BitWidth) {
    return ConstantRange::getEmpty(BitWidth);
  }
  static ConstantRange getBestState(const IntegerRangeState &IRS) {
    return getBestState(IRS.getBitWidth());
  }

  /// Return associated values' bit width.
  uint32_t getBitWidth() const { return BitWidth; }

  /// See AbstractState::isValidState()
  bool isValidState() const override {
    return BitWidth > 0 && !Assumed.isFullSet();
  }

  /// See AbstractState::isAtFixpoint()
  bool isAtFixpoint() const override { return Assumed == Known; }

  /// See AbstractState::indicateOptimisticFixpoint(...)
  ChangeStatus indicateOptimisticFixpoint() override {
    Known = Assumed;
    return ChangeStatus::CHANGED;
  }

  /// See AbstractState::indicatePessimisticFixpoint(...)
  ChangeStatus indicatePessimisticFixpoint() override {
    Assumed = Known;
    return ChangeStatus::CHANGED;
  }

  /// Return the known state encoding
  ConstantRange getKnown() const { return Known; }

  /// Return the assumed state encoding.
  ConstantRange getAssumed() const { return Assumed; }

  /// Unite assumed range with the passed state.
  void unionAssumed(const ConstantRange &R) {
    // Don't loose a known range.
    Assumed = Assumed.unionWith(R).intersectWith(Known);
  }

  /// See IntegerRangeState::unionAssumed(..).
  void unionAssumed(const IntegerRangeState &R) {
    unionAssumed(R.getAssumed());
  }

  /// Unite known range with the passed state.
  void unionKnown(const ConstantRange &R) {
    // Don't loose a known range.
    Known = Known.unionWith(R);
    Assumed = Assumed.unionWith(Known);
  }

  /// See IntegerRangeState::unionKnown(..).
  void unionKnown(const IntegerRangeState &R) { unionKnown(R.getKnown()); }

  /// Intersect known range with the passed state.
  void intersectKnown(const ConstantRange &R) {
    Assumed = Assumed.intersectWith(R);
    Known = Known.intersectWith(R);
  }

  /// See IntegerRangeState::intersectKnown(..).
  void intersectKnown(const IntegerRangeState &R) {
    intersectKnown(R.getKnown());
  }

  /// Equality for IntegerRangeState.
  bool operator==(const IntegerRangeState &R) const {
    return getAssumed() == R.getAssumed() && getKnown() == R.getKnown();
  }

  /// "Clamp" this state with \p R. The result is subtype dependent but it is
  /// intended that only information assumed in both states will be assumed in
  /// this one afterwards.
  IntegerRangeState operator^=(const IntegerRangeState &R) {
    // NOTE: `^=` operator seems like `intersect` but in this case, we need to
    // take `union`.
    unionAssumed(R);
    return *this;
  }

  IntegerRangeState operator&=(const IntegerRangeState &R) {
    // NOTE: `&=` operator seems like `intersect` but in this case, we need to
    // take `union`.
    unionKnown(R);
    unionAssumed(R);
    return *this;
  }
};
/// Helper struct necessary as the modular build fails if the virtual method
/// IRAttribute::manifest is defined in the Attributor.cpp.
struct IRAttributeManifest {
  static ChangeStatus manifestAttrs(Attributor &A, const IRPosition &IRP,
                                    const ArrayRef<Attribute> &DeducedAttrs);
};

/// Helper to tie a abstract state implementation to an abstract attribute.
template <typename StateTy, typename BaseType, class... Ts>
struct StateWrapper : public BaseType, public StateTy {
  /// Provide static access to the type of the state.
  using StateType = StateTy;

  StateWrapper(const IRPosition &IRP, Ts... Args)
      : BaseType(IRP), StateTy(Args...) {}

  /// See AbstractAttribute::getState(...).
  StateType &getState() override { return *this; }

  /// See AbstractAttribute::getState(...).
  const StateType &getState() const override { return *this; }
};

/// Helper class that provides common functionality to manifest IR attributes.
template <Attribute::AttrKind AK, typename BaseType>
struct IRAttribute : public BaseType {
  IRAttribute(const IRPosition &IRP) : BaseType(IRP) {}

  /// See AbstractAttribute::initialize(...).
  virtual void initialize(Attributor &A) override {
    const IRPosition &IRP = this->getIRPosition();
    if (isa<UndefValue>(IRP.getAssociatedValue()) ||
        this->hasAttr(getAttrKind(), /* IgnoreSubsumingPositions */ false,
                      &A)) {
      this->getState().indicateOptimisticFixpoint();
      return;
    }

    bool IsFnInterface = IRP.isFnInterfaceKind();
    const Function *FnScope = IRP.getAnchorScope();
    // TODO: Not all attributes require an exact definition. Find a way to
    //       enable deduction for some but not all attributes in case the
    //       definition might be changed at runtime, see also
    //       http://lists.llvm.org/pipermail/llvm-dev/2018-February/121275.html.
    // TODO: We could always determine abstract attributes and if sufficient
    //       information was found we could duplicate the functions that do not
    //       have an exact definition.
    if (IsFnInterface && (!FnScope || !A.isFunctionIPOAmendable(*FnScope)))
      this->getState().indicatePessimisticFixpoint();
  }

  /// See AbstractAttribute::manifest(...).
  ChangeStatus manifest(Attributor &A) override {
    if (isa<UndefValue>(this->getIRPosition().getAssociatedValue()))
      return ChangeStatus::UNCHANGED;
    SmallVector<Attribute, 4> DeducedAttrs;
    getDeducedAttributes(this->getAnchorValue().getContext(), DeducedAttrs);
    return IRAttributeManifest::manifestAttrs(A, this->getIRPosition(),
                                              DeducedAttrs);
  }

  /// Return the kind that identifies the abstract attribute implementation.
  Attribute::AttrKind getAttrKind() const { return AK; }

  /// Return the deduced attributes in \p Attrs.
  virtual void getDeducedAttributes(LLVMContext &Ctx,
                                    SmallVectorImpl<Attribute> &Attrs) const {
    Attrs.emplace_back(Attribute::get(Ctx, getAttrKind()));
  }
};

/// Base struct for all "concrete attribute" deductions.
///
/// The abstract attribute is a minimal interface that allows the Attributor to
/// orchestrate the abstract/fixpoint analysis. The design allows to hide away
/// implementation choices made for the subclasses but also to structure their
/// implementation and simplify the use of other abstract attributes in-flight.
///
/// To allow easy creation of new attributes, most methods have default
/// implementations. The ones that do not are generally straight forward, except
/// `AbstractAttribute::updateImpl` which is the location of most reasoning
/// associated with the abstract attribute. The update is invoked by the
/// Attributor in case the situation used to justify the current optimistic
/// state might have changed. The Attributor determines this automatically
/// by monitoring the `Attributor::getAAFor` calls made by abstract attributes.
///
/// The `updateImpl` method should inspect the IR and other abstract attributes
/// in-flight to justify the best possible (=optimistic) state. The actual
/// implementation is, similar to the underlying abstract state encoding, not
/// exposed. In the most common case, the `updateImpl` will go through a list of
/// reasons why its optimistic state is valid given the current information. If
/// any combination of them holds and is sufficient to justify the current
/// optimistic state, the method shall return UNCHAGED. If not, the optimistic
/// state is adjusted to the situation and the method shall return CHANGED.
///
/// If the manifestation of the "concrete attribute" deduced by the subclass
/// differs from the "default" behavior, which is a (set of) LLVM-IR
/// attribute(s) for an argument, call site argument, function return value, or
/// function, the `AbstractAttribute::manifest` method should be overloaded.
///
/// NOTE: If the state obtained via getState() is INVALID, thus if
///       AbstractAttribute::getState().isValidState() returns false, no
///       information provided by the methods of this class should be used.
/// NOTE: The Attributor currently has certain limitations to what we can do.
///       As a general rule of thumb, "concrete" abstract attributes should *for
///       now* only perform "backward" information propagation. That means
///       optimistic information obtained through abstract attributes should
///       only be used at positions that precede the origin of the information
///       with regards to the program flow. More practically, information can
///       *now* be propagated from instructions to their enclosing function, but
///       *not* from call sites to the called function. The mechanisms to allow
///       both directions will be added in the future.
/// NOTE: The mechanics of adding a new "concrete" abstract attribute are
///       described in the file comment.
struct AbstractAttribute : public IRPosition, public AADepGraphNode {
  using StateType = AbstractState;

  AbstractAttribute(const IRPosition &IRP) : IRPosition(IRP) {}

  /// Virtual destructor.
  virtual ~AbstractAttribute() {}

  /// This function is used to identify if an \p DGN is of type
  /// AbstractAttribute so that the dyn_cast and cast can use such information
  /// to cast an AADepGraphNode to an AbstractAttribute.
  ///
  /// We eagerly return true here because all AADepGraphNodes except for the
  /// Synthethis Node are of type AbstractAttribute
  static bool classof(const AADepGraphNode *DGN) { return true; }

  /// Initialize the state with the information in the Attributor \p A.
  ///
  /// This function is called by the Attributor once all abstract attributes
  /// have been identified. It can and shall be used for task like:
  ///  - identify existing knowledge in the IR and use it for the "known state"
  ///  - perform any work that is not going to change over time, e.g., determine
  ///    a subset of the IR, or attributes in-flight, that have to be looked at
  ///    in the `updateImpl` method.
  virtual void initialize(Attributor &A) {}

  /// Return the internal abstract state for inspection.
  virtual StateType &getState() = 0;
  virtual const StateType &getState() const = 0;

  /// Return an IR position, see struct IRPosition.
  const IRPosition &getIRPosition() const { return *this; };
  IRPosition &getIRPosition() { return *this; };

  /// Helper functions, for debug purposes only.
  ///{
  void print(raw_ostream &OS) const override;
  virtual void printWithDeps(raw_ostream &OS) const;
  void dump() const { print(dbgs()); }

  /// This function should return the "summarized" assumed state as string.
  virtual const std::string getAsStr() const = 0;

  /// This function should return the name of the AbstractAttribute
  virtual const std::string getName() const = 0;

  /// This function should return the address of the ID of the AbstractAttribute
  virtual const char *getIdAddr() const = 0;
  ///}

  /// Allow the Attributor access to the protected methods.
  friend struct Attributor;

protected:
  /// Hook for the Attributor to trigger an update of the internal state.
  ///
  /// If this attribute is already fixed, this method will return UNCHANGED,
  /// otherwise it delegates to `AbstractAttribute::updateImpl`.
  ///
  /// \Return CHANGED if the internal state changed, otherwise UNCHANGED.
  ChangeStatus update(Attributor &A);

  /// Hook for the Attributor to trigger the manifestation of the information
  /// represented by the abstract attribute in the LLVM-IR.
  ///
  /// \Return CHANGED if the IR was altered, otherwise UNCHANGED.
  virtual ChangeStatus manifest(Attributor &A) {
    return ChangeStatus::UNCHANGED;
  }

  /// Hook to enable custom statistic tracking, called after manifest that
  /// resulted in a change if statistics are enabled.
  ///
  /// We require subclasses to provide an implementation so we remember to
  /// add statistics for them.
  virtual void trackStatistics() const = 0;

  /// The actual update/transfer function which has to be implemented by the
  /// derived classes.
  ///
  /// If it is called, the environment has changed and we have to determine if
  /// the current information is still valid or adjust it otherwise.
  ///
  /// \Return CHANGED if the internal state changed, otherwise UNCHANGED.
  virtual ChangeStatus updateImpl(Attributor &A) = 0;
};

/// Forward declarations of output streams for debug purposes.
///
///{
raw_ostream &operator<<(raw_ostream &OS, const AbstractAttribute &AA);
raw_ostream &operator<<(raw_ostream &OS, ChangeStatus S);
raw_ostream &operator<<(raw_ostream &OS, IRPosition::Kind);
raw_ostream &operator<<(raw_ostream &OS, const IRPosition &);
raw_ostream &operator<<(raw_ostream &OS, const AbstractState &State);
template <typename base_ty, base_ty BestState, base_ty WorstState>
raw_ostream &
operator<<(raw_ostream &OS,
           const IntegerStateBase<base_ty, BestState, WorstState> &S) {
  return OS << "(" << S.getKnown() << "-" << S.getAssumed() << ")"
            << static_cast<const AbstractState &>(S);
}
raw_ostream &operator<<(raw_ostream &OS, const IntegerRangeState &State);
///}

struct AttributorPass : public PassInfoMixin<AttributorPass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};
struct AttributorCGSCCPass : public PassInfoMixin<AttributorCGSCCPass> {
  PreservedAnalyses run(LazyCallGraph::SCC &C, CGSCCAnalysisManager &AM,
                        LazyCallGraph &CG, CGSCCUpdateResult &UR);
};

Pass *createAttributorLegacyPass();
Pass *createAttributorCGSCCLegacyPass();

/// ----------------------------------------------------------------------------
///                       Abstract Attribute Classes
/// ----------------------------------------------------------------------------

/// An abstract attribute for the returned values of a function.
struct AAReturnedValues
    : public IRAttribute<Attribute::Returned, AbstractAttribute> {
  AAReturnedValues(const IRPosition &IRP, Attributor &A) : IRAttribute(IRP) {}

  /// Return an assumed unique return value if a single candidate is found. If
  /// there cannot be one, return a nullptr. If it is not clear yet, return the
  /// Optional::NoneType.
  Optional<Value *> getAssumedUniqueReturnValue(Attributor &A) const;

  /// Check \p Pred on all returned values.
  ///
  /// This method will evaluate \p Pred on returned values and return
  /// true if (1) all returned values are known, and (2) \p Pred returned true
  /// for all returned values.
  ///
  /// Note: Unlike the Attributor::checkForAllReturnedValuesAndReturnInsts
  /// method, this one will not filter dead return instructions.
  virtual bool checkForAllReturnedValuesAndReturnInsts(
      function_ref<bool(Value &, const SmallSetVector<ReturnInst *, 4> &)> Pred)
      const = 0;

  using iterator =
      MapVector<Value *, SmallSetVector<ReturnInst *, 4>>::iterator;
  using const_iterator =
      MapVector<Value *, SmallSetVector<ReturnInst *, 4>>::const_iterator;
  virtual llvm::iterator_range<iterator> returned_values() = 0;
  virtual llvm::iterator_range<const_iterator> returned_values() const = 0;

  virtual size_t getNumReturnValues() const = 0;
  virtual const SmallSetVector<CallBase *, 4> &getUnresolvedCalls() const = 0;

  /// Create an abstract attribute view for the position \p IRP.
  static AAReturnedValues &createForPosition(const IRPosition &IRP,
                                             Attributor &A);

  /// See AbstractAttribute::getName()
  const std::string getName() const override { return "AAReturnedValues"; }

  /// See AbstractAttribute::getIdAddr()
  const char *getIdAddr() const override { return &ID; }

  /// This function should return true if the type of the \p AA is
  /// AAReturnedValues
  static bool classof(const AbstractAttribute *AA) {
    return (AA->getIdAddr() == &ID);
  }

  /// Unique ID (due to the unique address)
  static const char ID;
};

struct AANoUnwind
    : public IRAttribute<Attribute::NoUnwind,
                         StateWrapper<BooleanState, AbstractAttribute>> {
  AANoUnwind(const IRPosition &IRP, Attributor &A) : IRAttribute(IRP) {}

  /// Returns true if nounwind is assumed.
  bool isAssumedNoUnwind() const { return getAssumed(); }

  /// Returns true if nounwind is known.
  bool isKnownNoUnwind() const { return getKnown(); }

  /// Create an abstract attribute view for the position \p IRP.
  static AANoUnwind &createForPosition(const IRPosition &IRP, Attributor &A);

  /// See AbstractAttribute::getName()
  const std::string getName() const override { return "AANoUnwind"; }

  /// See AbstractAttribute::getIdAddr()
  const char *getIdAddr() const override { return &ID; }

  /// This function should return true if the type of the \p AA is AANoUnwind
  static bool classof(const AbstractAttribute *AA) {
    return (AA->getIdAddr() == &ID);
  }

  /// Unique ID (due to the unique address)
  static const char ID;
};

struct AANoSync
    : public IRAttribute<Attribute::NoSync,
                         StateWrapper<BooleanState, AbstractAttribute>> {
  AANoSync(const IRPosition &IRP, Attributor &A) : IRAttribute(IRP) {}

  /// Returns true if "nosync" is assumed.
  bool isAssumedNoSync() const { return getAssumed(); }

  /// Returns true if "nosync" is known.
  bool isKnownNoSync() const { return getKnown(); }

  /// Create an abstract attribute view for the position \p IRP.
  static AANoSync &createForPosition(const IRPosition &IRP, Attributor &A);

  /// See AbstractAttribute::getName()
  const std::string getName() const override { return "AANoSync"; }

  /// See AbstractAttribute::getIdAddr()
  const char *getIdAddr() const override { return &ID; }

  /// This function should return true if the type of the \p AA is AANoSync
  static bool classof(const AbstractAttribute *AA) {
    return (AA->getIdAddr() == &ID);
  }

  /// Unique ID (due to the unique address)
  static const char ID;
};

/// An abstract interface for all nonnull attributes.
struct AANonNull
    : public IRAttribute<Attribute::NonNull,
                         StateWrapper<BooleanState, AbstractAttribute>> {
  AANonNull(const IRPosition &IRP, Attributor &A) : IRAttribute(IRP) {}

  /// Return true if we assume that the underlying value is nonnull.
  bool isAssumedNonNull() const { return getAssumed(); }

  /// Return true if we know that underlying value is nonnull.
  bool isKnownNonNull() const { return getKnown(); }

  /// Create an abstract attribute view for the position \p IRP.
  static AANonNull &createForPosition(const IRPosition &IRP, Attributor &A);

  /// See AbstractAttribute::getName()
  const std::string getName() const override { return "AANonNull"; }

  /// See AbstractAttribute::getIdAddr()
  const char *getIdAddr() const override { return &ID; }

  /// This function should return true if the type of the \p AA is AANonNull
  static bool classof(const AbstractAttribute *AA) {
    return (AA->getIdAddr() == &ID);
  }

  /// Unique ID (due to the unique address)
  static const char ID;
};

/// An abstract attribute for norecurse.
struct AANoRecurse
    : public IRAttribute<Attribute::NoRecurse,
                         StateWrapper<BooleanState, AbstractAttribute>> {
  AANoRecurse(const IRPosition &IRP, Attributor &A) : IRAttribute(IRP) {}

  /// Return true if "norecurse" is assumed.
  bool isAssumedNoRecurse() const { return getAssumed(); }

  /// Return true if "norecurse" is known.
  bool isKnownNoRecurse() const { return getKnown(); }

  /// Create an abstract attribute view for the position \p IRP.
  static AANoRecurse &createForPosition(const IRPosition &IRP, Attributor &A);

  /// See AbstractAttribute::getName()
  const std::string getName() const override { return "AANoRecurse"; }

  /// See AbstractAttribute::getIdAddr()
  const char *getIdAddr() const override { return &ID; }

  /// This function should return true if the type of the \p AA is AANoRecurse
  static bool classof(const AbstractAttribute *AA) {
    return (AA->getIdAddr() == &ID);
  }

  /// Unique ID (due to the unique address)
  static const char ID;
};

/// An abstract attribute for willreturn.
struct AAWillReturn
    : public IRAttribute<Attribute::WillReturn,
                         StateWrapper<BooleanState, AbstractAttribute>> {
  AAWillReturn(const IRPosition &IRP, Attributor &A) : IRAttribute(IRP) {}

  /// Return true if "willreturn" is assumed.
  bool isAssumedWillReturn() const { return getAssumed(); }

  /// Return true if "willreturn" is known.
  bool isKnownWillReturn() const { return getKnown(); }

  /// Create an abstract attribute view for the position \p IRP.
  static AAWillReturn &createForPosition(const IRPosition &IRP, Attributor &A);

  /// See AbstractAttribute::getName()
  const std::string getName() const override { return "AAWillReturn"; }

  /// See AbstractAttribute::getIdAddr()
  const char *getIdAddr() const override { return &ID; }

  /// This function should return true if the type of the \p AA is AAWillReturn
  static bool classof(const AbstractAttribute *AA) {
    return (AA->getIdAddr() == &ID);
  }

  /// Unique ID (due to the unique address)
  static const char ID;
};

/// An abstract attribute for undefined behavior.
struct AAUndefinedBehavior
    : public StateWrapper<BooleanState, AbstractAttribute> {
  using Base = StateWrapper<BooleanState, AbstractAttribute>;
  AAUndefinedBehavior(const IRPosition &IRP, Attributor &A) : Base(IRP) {}

  /// Return true if "undefined behavior" is assumed.
  bool isAssumedToCauseUB() const { return getAssumed(); }

  /// Return true if "undefined behavior" is assumed for a specific instruction.
  virtual bool isAssumedToCauseUB(Instruction *I) const = 0;

  /// Return true if "undefined behavior" is known.
  bool isKnownToCauseUB() const { return getKnown(); }

  /// Return true if "undefined behavior" is known for a specific instruction.
  virtual bool isKnownToCauseUB(Instruction *I) const = 0;

  /// Create an abstract attribute view for the position \p IRP.
  static AAUndefinedBehavior &createForPosition(const IRPosition &IRP,
                                                Attributor &A);

  /// See AbstractAttribute::getName()
  const std::string getName() const override { return "AAUndefinedBehavior"; }

  /// See AbstractAttribute::getIdAddr()
  const char *getIdAddr() const override { return &ID; }

  /// This function should return true if the type of the \p AA is
  /// AAUndefineBehavior
  static bool classof(const AbstractAttribute *AA) {
    return (AA->getIdAddr() == &ID);
  }

  /// Unique ID (due to the unique address)
  static const char ID;
};

/// An abstract interface to determine reachability of point A to B.
struct AAReachability : public StateWrapper<BooleanState, AbstractAttribute> {
  using Base = StateWrapper<BooleanState, AbstractAttribute>;
  AAReachability(const IRPosition &IRP, Attributor &A) : Base(IRP) {}

  /// Returns true if 'From' instruction is assumed to reach, 'To' instruction.
  /// Users should provide two positions they are interested in, and the class
  /// determines (and caches) reachability.
  bool isAssumedReachable(Attributor &A, const Instruction &From,
                          const Instruction &To) const {
    return A.getInfoCache().getPotentiallyReachable(From, To);
  }

  /// Returns true if 'From' instruction is known to reach, 'To' instruction.
  /// Users should provide two positions they are interested in, and the class
  /// determines (and caches) reachability.
  bool isKnownReachable(Attributor &A, const Instruction &From,
                        const Instruction &To) const {
    return A.getInfoCache().getPotentiallyReachable(From, To);
  }

  /// Create an abstract attribute view for the position \p IRP.
  static AAReachability &createForPosition(const IRPosition &IRP,
                                           Attributor &A);

  /// See AbstractAttribute::getName()
  const std::string getName() const override { return "AAReachability"; }

  /// See AbstractAttribute::getIdAddr()
  const char *getIdAddr() const override { return &ID; }

  /// This function should return true if the type of the \p AA is
  /// AAReachability
  static bool classof(const AbstractAttribute *AA) {
    return (AA->getIdAddr() == &ID);
  }

  /// Unique ID (due to the unique address)
  static const char ID;
};

/// An abstract interface for all noalias attributes.
struct AANoAlias
    : public IRAttribute<Attribute::NoAlias,
                         StateWrapper<BooleanState, AbstractAttribute>> {
  AANoAlias(const IRPosition &IRP, Attributor &A) : IRAttribute(IRP) {}

  /// Return true if we assume that the underlying value is alias.
  bool isAssumedNoAlias() const { return getAssumed(); }

  /// Return true if we know that underlying value is noalias.
  bool isKnownNoAlias() const { return getKnown(); }

  /// Create an abstract attribute view for the position \p IRP.
  static AANoAlias &createForPosition(const IRPosition &IRP, Attributor &A);

  /// See AbstractAttribute::getName()
  const std::string getName() const override { return "AANoAlias"; }

  /// See AbstractAttribute::getIdAddr()
  const char *getIdAddr() const override { return &ID; }

  /// This function should return true if the type of the \p AA is AANoAlias
  static bool classof(const AbstractAttribute *AA) {
    return (AA->getIdAddr() == &ID);
  }

  /// Unique ID (due to the unique address)
  static const char ID;
};

/// An AbstractAttribute for nofree.
struct AANoFree
    : public IRAttribute<Attribute::NoFree,
                         StateWrapper<BooleanState, AbstractAttribute>> {
  AANoFree(const IRPosition &IRP, Attributor &A) : IRAttribute(IRP) {}

  /// Return true if "nofree" is assumed.
  bool isAssumedNoFree() const { return getAssumed(); }

  /// Return true if "nofree" is known.
  bool isKnownNoFree() const { return getKnown(); }

  /// Create an abstract attribute view for the position \p IRP.
  static AANoFree &createForPosition(const IRPosition &IRP, Attributor &A);

  /// See AbstractAttribute::getName()
  const std::string getName() const override { return "AANoFree"; }

  /// See AbstractAttribute::getIdAddr()
  const char *getIdAddr() const override { return &ID; }

  /// This function should return true if the type of the \p AA is AANoFree
  static bool classof(const AbstractAttribute *AA) {
    return (AA->getIdAddr() == &ID);
  }

  /// Unique ID (due to the unique address)
  static const char ID;
};

/// An AbstractAttribute for noreturn.
struct AANoReturn
    : public IRAttribute<Attribute::NoReturn,
                         StateWrapper<BooleanState, AbstractAttribute>> {
  AANoReturn(const IRPosition &IRP, Attributor &A) : IRAttribute(IRP) {}

  /// Return true if the underlying object is assumed to never return.
  bool isAssumedNoReturn() const { return getAssumed(); }

  /// Return true if the underlying object is known to never return.
  bool isKnownNoReturn() const { return getKnown(); }

  /// Create an abstract attribute view for the position \p IRP.
  static AANoReturn &createForPosition(const IRPosition &IRP, Attributor &A);

  /// See AbstractAttribute::getName()
  const std::string getName() const override { return "AANoReturn"; }

  /// See AbstractAttribute::getIdAddr()
  const char *getIdAddr() const override { return &ID; }

  /// This function should return true if the type of the \p AA is AANoReturn
  static bool classof(const AbstractAttribute *AA) {
    return (AA->getIdAddr() == &ID);
  }

  /// Unique ID (due to the unique address)
  static const char ID;
};

/// An abstract interface for liveness abstract attribute.
struct AAIsDead : public StateWrapper<BooleanState, AbstractAttribute> {
  using Base = StateWrapper<BooleanState, AbstractAttribute>;
  AAIsDead(const IRPosition &IRP, Attributor &A) : Base(IRP) {}

protected:
  /// The query functions are protected such that other attributes need to go
  /// through the Attributor interfaces: `Attributor::isAssumedDead(...)`

  /// Returns true if the underlying value is assumed dead.
  virtual bool isAssumedDead() const = 0;

  /// Returns true if the underlying value is known dead.
  virtual bool isKnownDead() const = 0;

  /// Returns true if \p BB is assumed dead.
  virtual bool isAssumedDead(const BasicBlock *BB) const = 0;

  /// Returns true if \p BB is known dead.
  virtual bool isKnownDead(const BasicBlock *BB) const = 0;

  /// Returns true if \p I is assumed dead.
  virtual bool isAssumedDead(const Instruction *I) const = 0;

  /// Returns true if \p I is known dead.
  virtual bool isKnownDead(const Instruction *I) const = 0;

  /// This method is used to check if at least one instruction in a collection
  /// of instructions is live.
  template <typename T> bool isLiveInstSet(T begin, T end) const {
    for (const auto &I : llvm::make_range(begin, end)) {
      assert(I->getFunction() == getIRPosition().getAssociatedFunction() &&
             "Instruction must be in the same anchor scope function.");

      if (!isAssumedDead(I))
        return true;
    }

    return false;
  }

public:
  /// Create an abstract attribute view for the position \p IRP.
  static AAIsDead &createForPosition(const IRPosition &IRP, Attributor &A);

  /// Determine if \p F might catch asynchronous exceptions.
  static bool mayCatchAsynchronousExceptions(const Function &F) {
    return F.hasPersonalityFn() && !canSimplifyInvokeNoUnwind(&F);
  }

  /// Return if the edge from \p From BB to \p To BB is assumed dead.
  /// This is specifically useful in AAReachability.
  virtual bool isEdgeDead(const BasicBlock *From, const BasicBlock *To) const {
    return false;
  }

  /// See AbstractAttribute::getName()
  const std::string getName() const override { return "AAIsDead"; }

  /// See AbstractAttribute::getIdAddr()
  const char *getIdAddr() const override { return &ID; }

  /// This function should return true if the type of the \p AA is AAIsDead
  static bool classof(const AbstractAttribute *AA) {
    return (AA->getIdAddr() == &ID);
  }

  /// Unique ID (due to the unique address)
  static const char ID;

  friend struct Attributor;
};

/// State for dereferenceable attribute
struct DerefState : AbstractState {

  static DerefState getBestState() { return DerefState(); }
  static DerefState getBestState(const DerefState &) { return getBestState(); }

  /// Return the worst possible representable state.
  static DerefState getWorstState() {
    DerefState DS;
    DS.indicatePessimisticFixpoint();
    return DS;
  }
  static DerefState getWorstState(const DerefState &) {
    return getWorstState();
  }

  /// State representing for dereferenceable bytes.
  IncIntegerState<> DerefBytesState;

  /// Map representing for accessed memory offsets and sizes.
  /// A key is Offset and a value is size.
  /// If there is a load/store instruction something like,
  ///   p[offset] = v;
  /// (offset, sizeof(v)) will be inserted to this map.
  /// std::map is used because we want to iterate keys in ascending order.
  std::map<int64_t, uint64_t> AccessedBytesMap;

  /// Helper function to calculate dereferenceable bytes from current known
  /// bytes and accessed bytes.
  ///
  /// int f(int *A){
  ///    *A = 0;
  ///    *(A+2) = 2;
  ///    *(A+1) = 1;
  ///    *(A+10) = 10;
  /// }
  /// ```
  /// In that case, AccessedBytesMap is `{0:4, 4:4, 8:4, 40:4}`.
  /// AccessedBytesMap is std::map so it is iterated in accending order on
  /// key(Offset). So KnownBytes will be updated like this:
  ///
  /// |Access | KnownBytes
  /// |(0, 4)| 0 -> 4
  /// |(4, 4)| 4 -> 8
  /// |(8, 4)| 8 -> 12
  /// |(40, 4) | 12 (break)
  void computeKnownDerefBytesFromAccessedMap() {
    int64_t KnownBytes = DerefBytesState.getKnown();
    for (auto &Access : AccessedBytesMap) {
      if (KnownBytes < Access.first)
        break;
      KnownBytes = std::max(KnownBytes, Access.first + (int64_t)Access.second);
    }

    DerefBytesState.takeKnownMaximum(KnownBytes);
  }

  /// State representing that whether the value is globaly dereferenceable.
  BooleanState GlobalState;

  /// See AbstractState::isValidState()
  bool isValidState() const override { return DerefBytesState.isValidState(); }

  /// See AbstractState::isAtFixpoint()
  bool isAtFixpoint() const override {
    return !isValidState() ||
           (DerefBytesState.isAtFixpoint() && GlobalState.isAtFixpoint());
  }

  /// See AbstractState::indicateOptimisticFixpoint(...)
  ChangeStatus indicateOptimisticFixpoint() override {
    DerefBytesState.indicateOptimisticFixpoint();
    GlobalState.indicateOptimisticFixpoint();
    return ChangeStatus::UNCHANGED;
  }

  /// See AbstractState::indicatePessimisticFixpoint(...)
  ChangeStatus indicatePessimisticFixpoint() override {
    DerefBytesState.indicatePessimisticFixpoint();
    GlobalState.indicatePessimisticFixpoint();
    return ChangeStatus::CHANGED;
  }

  /// Update known dereferenceable bytes.
  void takeKnownDerefBytesMaximum(uint64_t Bytes) {
    DerefBytesState.takeKnownMaximum(Bytes);

    // Known bytes might increase.
    computeKnownDerefBytesFromAccessedMap();
  }

  /// Update assumed dereferenceable bytes.
  void takeAssumedDerefBytesMinimum(uint64_t Bytes) {
    DerefBytesState.takeAssumedMinimum(Bytes);
  }

  /// Add accessed bytes to the map.
  void addAccessedBytes(int64_t Offset, uint64_t Size) {
    uint64_t &AccessedBytes = AccessedBytesMap[Offset];
    AccessedBytes = std::max(AccessedBytes, Size);

    // Known bytes might increase.
    computeKnownDerefBytesFromAccessedMap();
  }

  /// Equality for DerefState.
  bool operator==(const DerefState &R) const {
    return this->DerefBytesState == R.DerefBytesState &&
           this->GlobalState == R.GlobalState;
  }

  /// Inequality for DerefState.
  bool operator!=(const DerefState &R) const { return !(*this == R); }

  /// See IntegerStateBase::operator^=
  DerefState operator^=(const DerefState &R) {
    DerefBytesState ^= R.DerefBytesState;
    GlobalState ^= R.GlobalState;
    return *this;
  }

  /// See IntegerStateBase::operator+=
  DerefState operator+=(const DerefState &R) {
    DerefBytesState += R.DerefBytesState;
    GlobalState += R.GlobalState;
    return *this;
  }

  /// See IntegerStateBase::operator&=
  DerefState operator&=(const DerefState &R) {
    DerefBytesState &= R.DerefBytesState;
    GlobalState &= R.GlobalState;
    return *this;
  }

  /// See IntegerStateBase::operator|=
  DerefState operator|=(const DerefState &R) {
    DerefBytesState |= R.DerefBytesState;
    GlobalState |= R.GlobalState;
    return *this;
  }

protected:
  const AANonNull *NonNullAA = nullptr;
};

/// An abstract interface for all dereferenceable attribute.
struct AADereferenceable
    : public IRAttribute<Attribute::Dereferenceable,
                         StateWrapper<DerefState, AbstractAttribute>> {
  AADereferenceable(const IRPosition &IRP, Attributor &A) : IRAttribute(IRP) {}

  /// Return true if we assume that the underlying value is nonnull.
  bool isAssumedNonNull() const {
    return NonNullAA && NonNullAA->isAssumedNonNull();
  }

  /// Return true if we know that the underlying value is nonnull.
  bool isKnownNonNull() const {
    return NonNullAA && NonNullAA->isKnownNonNull();
  }

  /// Return true if we assume that underlying value is
  /// dereferenceable(_or_null) globally.
  bool isAssumedGlobal() const { return GlobalState.getAssumed(); }

  /// Return true if we know that underlying value is
  /// dereferenceable(_or_null) globally.
  bool isKnownGlobal() const { return GlobalState.getKnown(); }

  /// Return assumed dereferenceable bytes.
  uint32_t getAssumedDereferenceableBytes() const {
    return DerefBytesState.getAssumed();
  }

  /// Return known dereferenceable bytes.
  uint32_t getKnownDereferenceableBytes() const {
    return DerefBytesState.getKnown();
  }

  /// Create an abstract attribute view for the position \p IRP.
  static AADereferenceable &createForPosition(const IRPosition &IRP,
                                              Attributor &A);

  /// See AbstractAttribute::getName()
  const std::string getName() const override { return "AADereferenceable"; }

  /// See AbstractAttribute::getIdAddr()
  const char *getIdAddr() const override { return &ID; }

  /// This function should return true if the type of the \p AA is
  /// AADereferenceable
  static bool classof(const AbstractAttribute *AA) {
    return (AA->getIdAddr() == &ID);
  }

  /// Unique ID (due to the unique address)
  static const char ID;
};

using AAAlignmentStateType =
    IncIntegerState<uint32_t, Value::MaximumAlignment, 1>;
/// An abstract interface for all align attributes.
struct AAAlign : public IRAttribute<
                     Attribute::Alignment,
                     StateWrapper<AAAlignmentStateType, AbstractAttribute>> {
  AAAlign(const IRPosition &IRP, Attributor &A) : IRAttribute(IRP) {}

  /// Return assumed alignment.
  unsigned getAssumedAlign() const { return getAssumed(); }

  /// Return known alignment.
  unsigned getKnownAlign() const { return getKnown(); }

  /// See AbstractAttribute::getName()
  const std::string getName() const override { return "AAAlign"; }

  /// See AbstractAttribute::getIdAddr()
  const char *getIdAddr() const override { return &ID; }

  /// This function should return true if the type of the \p AA is AAAlign
  static bool classof(const AbstractAttribute *AA) {
    return (AA->getIdAddr() == &ID);
  }

  /// Create an abstract attribute view for the position \p IRP.
  static AAAlign &createForPosition(const IRPosition &IRP, Attributor &A);

  /// Unique ID (due to the unique address)
  static const char ID;
};

/// An abstract interface for all nocapture attributes.
struct AANoCapture
    : public IRAttribute<
          Attribute::NoCapture,
          StateWrapper<BitIntegerState<uint16_t, 7, 0>, AbstractAttribute>> {
  AANoCapture(const IRPosition &IRP, Attributor &A) : IRAttribute(IRP) {}

  /// State encoding bits. A set bit in the state means the property holds.
  /// NO_CAPTURE is the best possible state, 0 the worst possible state.
  enum {
    NOT_CAPTURED_IN_MEM = 1 << 0,
    NOT_CAPTURED_IN_INT = 1 << 1,
    NOT_CAPTURED_IN_RET = 1 << 2,

    /// If we do not capture the value in memory or through integers we can only
    /// communicate it back as a derived pointer.
    NO_CAPTURE_MAYBE_RETURNED = NOT_CAPTURED_IN_MEM | NOT_CAPTURED_IN_INT,

    /// If we do not capture the value in memory, through integers, or as a
    /// derived pointer we know it is not captured.
    NO_CAPTURE =
        NOT_CAPTURED_IN_MEM | NOT_CAPTURED_IN_INT | NOT_CAPTURED_IN_RET,
  };

  /// Return true if we know that the underlying value is not captured in its
  /// respective scope.
  bool isKnownNoCapture() const { return isKnown(NO_CAPTURE); }

  /// Return true if we assume that the underlying value is not captured in its
  /// respective scope.
  bool isAssumedNoCapture() const { return isAssumed(NO_CAPTURE); }

  /// Return true if we know that the underlying value is not captured in its
  /// respective scope but we allow it to escape through a "return".
  bool isKnownNoCaptureMaybeReturned() const {
    return isKnown(NO_CAPTURE_MAYBE_RETURNED);
  }

  /// Return true if we assume that the underlying value is not captured in its
  /// respective scope but we allow it to escape through a "return".
  bool isAssumedNoCaptureMaybeReturned() const {
    return isAssumed(NO_CAPTURE_MAYBE_RETURNED);
  }

  /// Create an abstract attribute view for the position \p IRP.
  static AANoCapture &createForPosition(const IRPosition &IRP, Attributor &A);

  /// See AbstractAttribute::getName()
  const std::string getName() const override { return "AANoCapture"; }

  /// See AbstractAttribute::getIdAddr()
  const char *getIdAddr() const override { return &ID; }

  /// This function should return true if the type of the \p AA is AANoCapture
  static bool classof(const AbstractAttribute *AA) {
    return (AA->getIdAddr() == &ID);
  }

  /// Unique ID (due to the unique address)
  static const char ID;
};

/// An abstract interface for value simplify abstract attribute.
struct AAValueSimplify : public StateWrapper<BooleanState, AbstractAttribute> {
  using Base = StateWrapper<BooleanState, AbstractAttribute>;
  AAValueSimplify(const IRPosition &IRP, Attributor &A) : Base(IRP) {}

  /// Return an assumed simplified value if a single candidate is found. If
  /// there cannot be one, return original value. If it is not clear yet, return
  /// the Optional::NoneType.
  virtual Optional<Value *> getAssumedSimplifiedValue(Attributor &A) const = 0;

  /// Create an abstract attribute view for the position \p IRP.
  static AAValueSimplify &createForPosition(const IRPosition &IRP,
                                            Attributor &A);

  /// See AbstractAttribute::getName()
  const std::string getName() const override { return "AAValueSimplify"; }

  /// See AbstractAttribute::getIdAddr()
  const char *getIdAddr() const override { return &ID; }

  /// This function should return true if the type of the \p AA is
  /// AAValueSimplify
  static bool classof(const AbstractAttribute *AA) {
    return (AA->getIdAddr() == &ID);
  }

  /// Unique ID (due to the unique address)
  static const char ID;
};

struct AAHeapToStack : public StateWrapper<BooleanState, AbstractAttribute> {
  using Base = StateWrapper<BooleanState, AbstractAttribute>;
  AAHeapToStack(const IRPosition &IRP, Attributor &A) : Base(IRP) {}

  /// Returns true if HeapToStack conversion is assumed to be possible.
  bool isAssumedHeapToStack() const { return getAssumed(); }

  /// Returns true if HeapToStack conversion is known to be possible.
  bool isKnownHeapToStack() const { return getKnown(); }

  /// Create an abstract attribute view for the position \p IRP.
  static AAHeapToStack &createForPosition(const IRPosition &IRP, Attributor &A);

  /// See AbstractAttribute::getName()
  const std::string getName() const override { return "AAHeapToStack"; }

  /// See AbstractAttribute::getIdAddr()
  const char *getIdAddr() const override { return &ID; }

  /// This function should return true if the type of the \p AA is AAHeapToStack
  static bool classof(const AbstractAttribute *AA) {
    return (AA->getIdAddr() == &ID);
  }

  /// Unique ID (due to the unique address)
  static const char ID;
};

/// An abstract interface for privatizability.
///
/// A pointer is privatizable if it can be replaced by a new, private one.
/// Privatizing pointer reduces the use count, interaction between unrelated
/// code parts.
///
/// In order for a pointer to be privatizable its value cannot be observed
/// (=nocapture), it is (for now) not written (=readonly & noalias), we know
/// what values are necessary to make the private copy look like the original
/// one, and the values we need can be loaded (=dereferenceable).
struct AAPrivatizablePtr
    : public StateWrapper<BooleanState, AbstractAttribute> {
  using Base = StateWrapper<BooleanState, AbstractAttribute>;
  AAPrivatizablePtr(const IRPosition &IRP, Attributor &A) : Base(IRP) {}

  /// Returns true if pointer privatization is assumed to be possible.
  bool isAssumedPrivatizablePtr() const { return getAssumed(); }

  /// Returns true if pointer privatization is known to be possible.
  bool isKnownPrivatizablePtr() const { return getKnown(); }

  /// Return the type we can choose for a private copy of the underlying
  /// value. None means it is not clear yet, nullptr means there is none.
  virtual Optional<Type *> getPrivatizableType() const = 0;

  /// Create an abstract attribute view for the position \p IRP.
  static AAPrivatizablePtr &createForPosition(const IRPosition &IRP,
                                              Attributor &A);

  /// See AbstractAttribute::getName()
  const std::string getName() const override { return "AAPrivatizablePtr"; }

  /// See AbstractAttribute::getIdAddr()
  const char *getIdAddr() const override { return &ID; }

  /// This function should return true if the type of the \p AA is
  /// AAPricatizablePtr
  static bool classof(const AbstractAttribute *AA) {
    return (AA->getIdAddr() == &ID);
  }

  /// Unique ID (due to the unique address)
  static const char ID;
};

/// An abstract interface for memory access kind related attributes
/// (readnone/readonly/writeonly).
struct AAMemoryBehavior
    : public IRAttribute<
          Attribute::ReadNone,
          StateWrapper<BitIntegerState<uint8_t, 3>, AbstractAttribute>> {
  AAMemoryBehavior(const IRPosition &IRP, Attributor &A) : IRAttribute(IRP) {}

  /// State encoding bits. A set bit in the state means the property holds.
  /// BEST_STATE is the best possible state, 0 the worst possible state.
  enum {
    NO_READS = 1 << 0,
    NO_WRITES = 1 << 1,
    NO_ACCESSES = NO_READS | NO_WRITES,

    BEST_STATE = NO_ACCESSES,
  };
  static_assert(BEST_STATE == getBestState(), "Unexpected BEST_STATE value");

  /// Return true if we know that the underlying value is not read or accessed
  /// in its respective scope.
  bool isKnownReadNone() const { return isKnown(NO_ACCESSES); }

  /// Return true if we assume that the underlying value is not read or accessed
  /// in its respective scope.
  bool isAssumedReadNone() const { return isAssumed(NO_ACCESSES); }

  /// Return true if we know that the underlying value is not accessed
  /// (=written) in its respective scope.
  bool isKnownReadOnly() const { return isKnown(NO_WRITES); }

  /// Return true if we assume that the underlying value is not accessed
  /// (=written) in its respective scope.
  bool isAssumedReadOnly() const { return isAssumed(NO_WRITES); }

  /// Return true if we know that the underlying value is not read in its
  /// respective scope.
  bool isKnownWriteOnly() const { return isKnown(NO_READS); }

  /// Return true if we assume that the underlying value is not read in its
  /// respective scope.
  bool isAssumedWriteOnly() const { return isAssumed(NO_READS); }

  /// Create an abstract attribute view for the position \p IRP.
  static AAMemoryBehavior &createForPosition(const IRPosition &IRP,
                                             Attributor &A);

  /// See AbstractAttribute::getName()
  const std::string getName() const override { return "AAMemoryBehavior"; }

  /// See AbstractAttribute::getIdAddr()
  const char *getIdAddr() const override { return &ID; }

  /// This function should return true if the type of the \p AA is
  /// AAMemoryBehavior
  static bool classof(const AbstractAttribute *AA) {
    return (AA->getIdAddr() == &ID);
  }

  /// Unique ID (due to the unique address)
  static const char ID;
};

/// An abstract interface for all memory location attributes
/// (readnone/argmemonly/inaccessiblememonly/inaccessibleorargmemonly).
struct AAMemoryLocation
    : public IRAttribute<
          Attribute::ReadNone,
          StateWrapper<BitIntegerState<uint32_t, 511>, AbstractAttribute>> {
  using MemoryLocationsKind = StateType::base_t;

  AAMemoryLocation(const IRPosition &IRP, Attributor &A) : IRAttribute(IRP) {}

  /// Encoding of different locations that could be accessed by a memory
  /// access.
  enum {
    ALL_LOCATIONS = 0,
    NO_LOCAL_MEM = 1 << 0,
    NO_CONST_MEM = 1 << 1,
    NO_GLOBAL_INTERNAL_MEM = 1 << 2,
    NO_GLOBAL_EXTERNAL_MEM = 1 << 3,
    NO_GLOBAL_MEM = NO_GLOBAL_INTERNAL_MEM | NO_GLOBAL_EXTERNAL_MEM,
    NO_ARGUMENT_MEM = 1 << 4,
    NO_INACCESSIBLE_MEM = 1 << 5,
    NO_MALLOCED_MEM = 1 << 6,
    NO_UNKOWN_MEM = 1 << 7,
    NO_LOCATIONS = NO_LOCAL_MEM | NO_CONST_MEM | NO_GLOBAL_INTERNAL_MEM |
                   NO_GLOBAL_EXTERNAL_MEM | NO_ARGUMENT_MEM |
                   NO_INACCESSIBLE_MEM | NO_MALLOCED_MEM | NO_UNKOWN_MEM,

    // Helper bit to track if we gave up or not.
    VALID_STATE = NO_LOCATIONS + 1,

    BEST_STATE = NO_LOCATIONS | VALID_STATE,
  };
  static_assert(BEST_STATE == getBestState(), "Unexpected BEST_STATE value");

  /// Return true if we know that the associated functions has no observable
  /// accesses.
  bool isKnownReadNone() const { return isKnown(NO_LOCATIONS); }

  /// Return true if we assume that the associated functions has no observable
  /// accesses.
  bool isAssumedReadNone() const {
    return isAssumed(NO_LOCATIONS) | isAssumedStackOnly();
  }

  /// Return true if we know that the associated functions has at most
  /// local/stack accesses.
  bool isKnowStackOnly() const {
    return isKnown(inverseLocation(NO_LOCAL_MEM, true, true));
  }

  /// Return true if we assume that the associated functions has at most
  /// local/stack accesses.
  bool isAssumedStackOnly() const {
    return isAssumed(inverseLocation(NO_LOCAL_MEM, true, true));
  }

  /// Return true if we know that the underlying value will only access
  /// inaccesible memory only (see Attribute::InaccessibleMemOnly).
  bool isKnownInaccessibleMemOnly() const {
    return isKnown(inverseLocation(NO_INACCESSIBLE_MEM, true, true));
  }

  /// Return true if we assume that the underlying value will only access
  /// inaccesible memory only (see Attribute::InaccessibleMemOnly).
  bool isAssumedInaccessibleMemOnly() const {
    return isAssumed(inverseLocation(NO_INACCESSIBLE_MEM, true, true));
  }

  /// Return true if we know that the underlying value will only access
  /// argument pointees (see Attribute::ArgMemOnly).
  bool isKnownArgMemOnly() const {
    return isKnown(inverseLocation(NO_ARGUMENT_MEM, true, true));
  }

  /// Return true if we assume that the underlying value will only access
  /// argument pointees (see Attribute::ArgMemOnly).
  bool isAssumedArgMemOnly() const {
    return isAssumed(inverseLocation(NO_ARGUMENT_MEM, true, true));
  }

  /// Return true if we know that the underlying value will only access
  /// inaccesible memory or argument pointees (see
  /// Attribute::InaccessibleOrArgMemOnly).
  bool isKnownInaccessibleOrArgMemOnly() const {
    return isKnown(
        inverseLocation(NO_INACCESSIBLE_MEM | NO_ARGUMENT_MEM, true, true));
  }

  /// Return true if we assume that the underlying value will only access
  /// inaccesible memory or argument pointees (see
  /// Attribute::InaccessibleOrArgMemOnly).
  bool isAssumedInaccessibleOrArgMemOnly() const {
    return isAssumed(
        inverseLocation(NO_INACCESSIBLE_MEM | NO_ARGUMENT_MEM, true, true));
  }

  /// Return true if the underlying value may access memory through arguement
  /// pointers of the associated function, if any.
  bool mayAccessArgMem() const { return !isAssumed(NO_ARGUMENT_MEM); }

  /// Return true if only the memory locations specififed by \p MLK are assumed
  /// to be accessed by the associated function.
  bool isAssumedSpecifiedMemOnly(MemoryLocationsKind MLK) const {
    return isAssumed(MLK);
  }

  /// Return the locations that are assumed to be not accessed by the associated
  /// function, if any.
  MemoryLocationsKind getAssumedNotAccessedLocation() const {
    return getAssumed();
  }

  /// Return the inverse of location \p Loc, thus for NO_XXX the return
  /// describes ONLY_XXX. The flags \p AndLocalMem and \p AndConstMem determine
  /// if local (=stack) and constant memory are allowed as well. Most of the
  /// time we do want them to be included, e.g., argmemonly allows accesses via
  /// argument pointers or local or constant memory accesses.
  static MemoryLocationsKind
  inverseLocation(MemoryLocationsKind Loc, bool AndLocalMem, bool AndConstMem) {
    return NO_LOCATIONS & ~(Loc | (AndLocalMem ? NO_LOCAL_MEM : 0) |
                            (AndConstMem ? NO_CONST_MEM : 0));
  };

  /// Return the locations encoded by \p MLK as a readable string.
  static std::string getMemoryLocationsAsStr(MemoryLocationsKind MLK);

  /// Simple enum to distinguish read/write/read-write accesses.
  enum AccessKind {
    NONE = 0,
    READ = 1 << 0,
    WRITE = 1 << 1,
    READ_WRITE = READ | WRITE,
  };

  /// Check \p Pred on all accesses to the memory kinds specified by \p MLK.
  ///
  /// This method will evaluate \p Pred on all accesses (access instruction +
  /// underlying accessed memory pointer) and it will return true if \p Pred
  /// holds every time.
  virtual bool checkForAllAccessesToMemoryKind(
      function_ref<bool(const Instruction *, const Value *, AccessKind,
                        MemoryLocationsKind)>
          Pred,
      MemoryLocationsKind MLK) const = 0;

  /// Create an abstract attribute view for the position \p IRP.
  static AAMemoryLocation &createForPosition(const IRPosition &IRP,
                                             Attributor &A);

  /// See AbstractState::getAsStr().
  const std::string getAsStr() const override {
    return getMemoryLocationsAsStr(getAssumedNotAccessedLocation());
  }

  /// See AbstractAttribute::getName()
  const std::string getName() const override { return "AAMemoryLocation"; }

  /// See AbstractAttribute::getIdAddr()
  const char *getIdAddr() const override { return &ID; }

  /// This function should return true if the type of the \p AA is
  /// AAMemoryLocation
  static bool classof(const AbstractAttribute *AA) {
    return (AA->getIdAddr() == &ID);
  }

  /// Unique ID (due to the unique address)
  static const char ID;
};

/// An abstract interface for range value analysis.
struct AAValueConstantRange
    : public StateWrapper<IntegerRangeState, AbstractAttribute, uint32_t> {
  using Base = StateWrapper<IntegerRangeState, AbstractAttribute, uint32_t>;
  AAValueConstantRange(const IRPosition &IRP, Attributor &A)
      : Base(IRP, IRP.getAssociatedType()->getIntegerBitWidth()) {}

  /// See AbstractAttribute::getState(...).
  IntegerRangeState &getState() override { return *this; }
  const IntegerRangeState &getState() const override { return *this; }

  /// Create an abstract attribute view for the position \p IRP.
  static AAValueConstantRange &createForPosition(const IRPosition &IRP,
                                                 Attributor &A);

  /// Return an assumed range for the assocaited value a program point \p CtxI.
  /// If \p I is nullptr, simply return an assumed range.
  virtual ConstantRange
  getAssumedConstantRange(Attributor &A,
                          const Instruction *CtxI = nullptr) const = 0;

  /// Return a known range for the assocaited value at a program point \p CtxI.
  /// If \p I is nullptr, simply return a known range.
  virtual ConstantRange
  getKnownConstantRange(Attributor &A,
                        const Instruction *CtxI = nullptr) const = 0;

  /// Return an assumed constant for the assocaited value a program point \p
  /// CtxI.
  Optional<ConstantInt *>
  getAssumedConstantInt(Attributor &A,
                        const Instruction *CtxI = nullptr) const {
    ConstantRange RangeV = getAssumedConstantRange(A, CtxI);
    if (auto *C = RangeV.getSingleElement())
      return cast<ConstantInt>(
          ConstantInt::get(getAssociatedValue().getType(), *C));
    if (RangeV.isEmptySet())
      return llvm::None;
    return nullptr;
  }

  /// See AbstractAttribute::getName()
  const std::string getName() const override { return "AAValueConstantRange"; }

  /// See AbstractAttribute::getIdAddr()
  const char *getIdAddr() const override { return &ID; }

  /// This function should return true if the type of the \p AA is
  /// AAValueConstantRange
  static bool classof(const AbstractAttribute *AA) {
    return (AA->getIdAddr() == &ID);
  }

  /// Unique ID (due to the unique address)
  static const char ID;
};

/// A class for a set state.
/// The assumed boolean state indicates whether the corresponding set is full
/// set or not. If the assumed state is false, this is the worst state. The
/// worst state (invalid state) of set of potential values is when the set
/// contains every possible value (i.e. we cannot in any way limit the value
/// that the target position can take). That never happens naturally, we only
/// force it. As for the conditions under which we force it, see
/// AAPotentialValues.
template <typename MemberTy, typename KeyInfo = DenseMapInfo<MemberTy>>
struct PotentialValuesState : AbstractState {
  using SetTy = DenseSet<MemberTy, KeyInfo>;

  PotentialValuesState() : IsValidState(true), UndefIsContained(false) {}

  PotentialValuesState(bool IsValid)
      : IsValidState(IsValid), UndefIsContained(false) {}

  /// See AbstractState::isValidState(...)
  bool isValidState() const override { return IsValidState.isValidState(); }

  /// See AbstractState::isAtFixpoint(...)
  bool isAtFixpoint() const override { return IsValidState.isAtFixpoint(); }

  /// See AbstractState::indicatePessimisticFixpoint(...)
  ChangeStatus indicatePessimisticFixpoint() override {
    return IsValidState.indicatePessimisticFixpoint();
  }

  /// See AbstractState::indicateOptimisticFixpoint(...)
  ChangeStatus indicateOptimisticFixpoint() override {
    return IsValidState.indicateOptimisticFixpoint();
  }

  /// Return the assumed state
  PotentialValuesState &getAssumed() { return *this; }
  const PotentialValuesState &getAssumed() const { return *this; }

  /// Return this set. We should check whether this set is valid or not by
  /// isValidState() before calling this function.
  const SetTy &getAssumedSet() const {
    assert(isValidState() && "This set shoud not be used when it is invalid!");
    return Set;
  }

  /// Returns whether this state contains an undef value or not.
  bool undefIsContained() const {
    assert(isValidState() && "This flag shoud not be used when it is invalid!");
    return UndefIsContained;
  }

  bool operator==(const PotentialValuesState &RHS) const {
    if (isValidState() != RHS.isValidState())
      return false;
    if (!isValidState() && !RHS.isValidState())
      return true;
    if (undefIsContained() != RHS.undefIsContained())
      return false;
    return Set == RHS.getAssumedSet();
  }

  /// Maximum number of potential values to be tracked.
  /// This is set by -attributor-max-potential-values command line option
  static unsigned MaxPotentialValues;

  /// Return empty set as the best state of potential values.
  static PotentialValuesState getBestState() {
    return PotentialValuesState(true);
  }

  static PotentialValuesState getBestState(PotentialValuesState &PVS) {
    return getBestState();
  }

  /// Return full set as the worst state of potential values.
  static PotentialValuesState getWorstState() {
    return PotentialValuesState(false);
  }

  /// Union assumed set with the passed value.
  void unionAssumed(const MemberTy &C) { insert(C); }

  /// Union assumed set with assumed set of the passed state \p PVS.
  void unionAssumed(const PotentialValuesState &PVS) { unionWith(PVS); }

  /// Union assumed set with an undef value.
  void unionAssumedWithUndef() { unionWithUndef(); }

  /// "Clamp" this state with \p PVS.
  PotentialValuesState operator^=(const PotentialValuesState &PVS) {
    IsValidState ^= PVS.IsValidState;
    unionAssumed(PVS);
    return *this;
  }

  PotentialValuesState operator&=(const PotentialValuesState &PVS) {
    IsValidState &= PVS.IsValidState;
    unionAssumed(PVS);
    return *this;
  }

private:
  /// Check the size of this set, and invalidate when the size is no
  /// less than \p MaxPotentialValues threshold.
  void checkAndInvalidate() {
    if (Set.size() >= MaxPotentialValues)
      indicatePessimisticFixpoint();
  }

  /// If this state contains both undef and not undef, we can reduce
  /// undef to the not undef value.
  void reduceUndefValue() { UndefIsContained = UndefIsContained & Set.empty(); }

  /// Insert an element into this set.
  void insert(const MemberTy &C) {
    if (!isValidState())
      return;
    Set.insert(C);
    checkAndInvalidate();
  }

  /// Take union with R.
  void unionWith(const PotentialValuesState &R) {
    /// If this is a full set, do nothing.;
    if (!isValidState())
      return;
    /// If R is full set, change L to a full set.
    if (!R.isValidState()) {
      indicatePessimisticFixpoint();
      return;
    }
    for (const MemberTy &C : R.Set)
      Set.insert(C);
    UndefIsContained |= R.undefIsContained();
    reduceUndefValue();
    checkAndInvalidate();
  }

  /// Take union with an undef value.
  void unionWithUndef() {
    UndefIsContained = true;
    reduceUndefValue();
  }

  /// Take intersection with R.
  void intersectWith(const PotentialValuesState &R) {
    /// If R is a full set, do nothing.
    if (!R.isValidState())
      return;
    /// If this is a full set, change this to R.
    if (!isValidState()) {
      *this = R;
      return;
    }
    SetTy IntersectSet;
    for (const MemberTy &C : Set) {
      if (R.Set.count(C))
        IntersectSet.insert(C);
    }
    Set = IntersectSet;
    UndefIsContained &= R.undefIsContained();
    reduceUndefValue();
  }

  /// A helper state which indicate whether this state is valid or not.
  BooleanState IsValidState;

  /// Container for potential values
  SetTy Set;

  /// Flag for undef value
  bool UndefIsContained;
};

using PotentialConstantIntValuesState = PotentialValuesState<APInt>;

raw_ostream &operator<<(raw_ostream &OS,
                        const PotentialConstantIntValuesState &R);

/// An abstract interface for potential values analysis.
///
/// This AA collects potential values for each IR position.
/// An assumed set of potential values is initialized with the empty set (the
/// best state) and it will grow monotonically as we find more potential values
/// for this position.
/// The set might be forced to the worst state, that is, to contain every
/// possible value for this position in 2 cases.
///   1. We surpassed the \p MaxPotentialValues threshold. This includes the
///      case that this position is affected (e.g. because of an operation) by a
///      Value that is in the worst state.
///   2. We tried to initialize on a Value that we cannot handle (e.g. an
///      operator we do not currently handle).
///
/// TODO: Support values other than constant integers.
struct AAPotentialValues
    : public StateWrapper<PotentialConstantIntValuesState, AbstractAttribute> {
  using Base = StateWrapper<PotentialConstantIntValuesState, AbstractAttribute>;
  AAPotentialValues(const IRPosition &IRP, Attributor &A) : Base(IRP) {}

  /// See AbstractAttribute::getState(...).
  PotentialConstantIntValuesState &getState() override { return *this; }
  const PotentialConstantIntValuesState &getState() const override {
    return *this;
  }

  /// Create an abstract attribute view for the position \p IRP.
  static AAPotentialValues &createForPosition(const IRPosition &IRP,
                                              Attributor &A);

  /// Return assumed constant for the associated value
  Optional<ConstantInt *>
  getAssumedConstantInt(Attributor &A,
                        const Instruction *CtxI = nullptr) const {
    if (!isValidState())
      return nullptr;
    if (getAssumedSet().size() == 1)
      return cast<ConstantInt>(ConstantInt::get(getAssociatedValue().getType(),
                                                *(getAssumedSet().begin())));
    if (getAssumedSet().size() == 0) {
      if (undefIsContained())
        return cast<ConstantInt>(
            ConstantInt::get(getAssociatedValue().getType(), 0));
      return llvm::None;
    }

    return nullptr;
  }

  /// See AbstractAttribute::getName()
  const std::string getName() const override { return "AAPotentialValues"; }

  /// See AbstractAttribute::getIdAddr()
  const char *getIdAddr() const override { return &ID; }

  /// This function should return true if the type of the \p AA is
  /// AAPotentialValues
  static bool classof(const AbstractAttribute *AA) {
    return (AA->getIdAddr() == &ID);
  }

  /// Unique ID (due to the unique address)
  static const char ID;
};

/// An abstract interface for all noundef attributes.
struct AANoUndef
    : public IRAttribute<Attribute::NoUndef,
                         StateWrapper<BooleanState, AbstractAttribute>> {
  AANoUndef(const IRPosition &IRP, Attributor &A) : IRAttribute(IRP) {}

  /// Return true if we assume that the underlying value is noundef.
  bool isAssumedNoUndef() const { return getAssumed(); }

  /// Return true if we know that underlying value is noundef.
  bool isKnownNoUndef() const { return getKnown(); }

  /// Create an abstract attribute view for the position \p IRP.
  static AANoUndef &createForPosition(const IRPosition &IRP, Attributor &A);

  /// See AbstractAttribute::getName()
  const std::string getName() const override { return "AANoUndef"; }

  /// See AbstractAttribute::getIdAddr()
  const char *getIdAddr() const override { return &ID; }

  /// This function should return true if the type of the \p AA is AANoUndef
  static bool classof(const AbstractAttribute *AA) {
    return (AA->getIdAddr() == &ID);
  }

  /// Unique ID (due to the unique address)
  static const char ID;
};

/// Run options, used by the pass manager.
enum AttributorRunOption {
  NONE = 0,
  MODULE = 1 << 0,
  CGSCC = 1 << 1,
  ALL = MODULE | CGSCC
};

} // end namespace llvm

#endif // LLVM_TRANSFORMS_IPO_ATTRIBUTOR_H
