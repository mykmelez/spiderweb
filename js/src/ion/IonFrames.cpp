/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=4 sw=4 et tw=79:
 *
 * ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is Mozilla Communicator client code, released
 * March 31, 1998.
 *
 * The Initial Developer of the Original Code is
 * Netscape Communications Corporation.
 * Portions created by the Initial Developer are Copyright (C) 1998
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   David Anderson <dvander@alliedmods.net>
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either of the GNU General Public License Version 2 or later (the "GPL"),
 * or the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#include "Ion.h"
#include "IonFrames.h"
#include "jsobj.h"
#include "jsscript.h"
#include "jsfun.h"
#include "IonCompartment.h"
#include "IonFrames-inl.h"
#include "IonFrameIterator-inl.h"
#include "Safepoints.h"
#include "IonSpewer.h"
#include "IonMacroAssembler.h"
#include "gc/Marking.h"
#include "SnapshotReader.h"
#include "Safepoints.h"
#include "VMFunctions.h"

using namespace js;
using namespace js::ion;

JSScript *
ion::MaybeScriptFromCalleeToken(CalleeToken token)
{
    switch (GetCalleeTokenTag(token)) {
      case CalleeToken_Script:
        return CalleeTokenToScript(token);
      case CalleeToken_Function:
        return CalleeTokenToFunction(token)->script();
    }
    JS_NOT_REACHED("invalid callee token tag");
    return NULL;
}

IonFrameIterator::IonFrameIterator(const IonActivationIterator &activations)
    : current_(activations.top()),
      type_(IonFrame_Exit),
      returnAddressToFp_(NULL),
      frameSize_(0),
      cachedSafepointIndex_(NULL),
      activation_(activations.activation())
{
}

IonFrameIterator::IonFrameIterator(IonJSFrameLayout *fp)
  : current_((uint8 *)fp),
    type_(IonFrame_JS),
    returnAddressToFp_(fp->returnAddress()),
    frameSize_(fp->prevFrameLocalSize())
{
}

bool
IonFrameIterator::checkInvalidation() const
{
    IonScript *dummy;
    return checkInvalidation(&dummy);
}

bool
IonFrameIterator::checkInvalidation(IonScript **ionScriptOut) const
{
    uint8 *returnAddr = returnAddressToFp();
    JSScript *script = this->script();
    // N.B. the current IonScript is not the same as the frame's
    // IonScript if the frame has since been invalidated.
    IonScript *currentIonScript = script->ion;
    bool invalidated = !script->hasIonScript() ||
        !currentIonScript->containsReturnAddress(returnAddr);
    if (!invalidated)
        return false;

    int32 invalidationDataOffset = ((int32 *) returnAddr)[-1];
    uint8 *ionScriptDataOffset = returnAddr + invalidationDataOffset;
    IonScript *ionScript = (IonScript *) Assembler::getPointer(ionScriptDataOffset);
    JS_ASSERT(ionScript->containsReturnAddress(returnAddr));
    *ionScriptOut = ionScript;
    return true;
}

CalleeToken
IonFrameIterator::calleeToken() const
{
    return ((IonJSFrameLayout *) current_)->calleeToken();
}

JSFunction *
IonFrameIterator::callee() const
{
    if (isScripted()) {
        JS_ASSERT(isFunctionFrame());
        return CalleeTokenToFunction(calleeToken());
    }

    JS_ASSERT(isNative());
    return exitFrame()->nativeExit()->vp()[0].toObject().toFunction();
}

JSFunction *
IonFrameIterator::maybeCallee() const
{
    if ((isScripted() && isFunctionFrame()) || isNative())
        return callee();
    return NULL;
}

bool
IonFrameIterator::isNative() const
{
    if (type_ != IonFrame_Exit)
        return false;
    return exitFrame()->footer()->ionCode() == NULL;
}

bool
IonFrameIterator::isFunctionFrame() const
{
    return js::ion::CalleeTokenIsFunction(calleeToken());
}

bool
IonFrameIterator::isEntryJSFrame() const
{
    if (prevType() == IonFrame_JS || prevType() == IonFrame_Bailed_JS)
        return false;

    if (prevType() == IonFrame_Entry)
        return true;

    IonFrameIterator iter(*this);
    ++iter;
    for (; !iter.done(); ++iter) {
        if (iter.isScripted())
            return false;
    }
    return true;
}

JSScript *
IonFrameIterator::script() const
{
    JS_ASSERT(isScripted());
    JSScript *script = MaybeScriptFromCalleeToken(calleeToken());
    JS_ASSERT(script);
    return script;
}

Value *
IonFrameIterator::nativeVp() const
{
    JS_ASSERT(isNative());
    return exitFrame()->nativeExit()->vp();
}

Value *
IonFrameIterator::actualArgs() const
{
    return jsFrame()->argv() + 1;
}

uint8 *
IonFrameIterator::prevFp() const
{
    JS_ASSERT(type_ != IonFrame_Entry);

    size_t currentSize = SizeOfFramePrefix(type_);
    // This quick fix must be removed as soon as bug 717297 land.  This is
    // needed because the descriptor size of JS-to-JS frame which is just after
    // a Rectifier frame should not change. (cf EnsureExitFrame function)
    if (prevType() == IonFrame_Bailed_Rectifier || prevType() == IonFrame_Bailed_JS) {
        JS_ASSERT(type_ == IonFrame_Exit);
        currentSize = SizeOfFramePrefix(IonFrame_JS);
    }
    currentSize += current()->prevFrameLocalSize();
    return current_ + currentSize;
}

IonFrameIterator &
IonFrameIterator::operator++()
{
    JS_ASSERT(type_ != IonFrame_Entry);

    frameSize_ = prevFrameLocalSize();
    cachedSafepointIndex_ = NULL;

    // If the next frame is the entry frame, just exit. Don't update current_,
    // since the entry and first frames overlap.
    if (current()->prevType() == IonFrame_Entry) {
        type_ = IonFrame_Entry;
        return *this;
    }

    // Note: prevFp() needs the current type, so set it after computing the
    // next frame.
    uint8 *prev = prevFp();
    type_ = current()->prevType();
    if (type_ == IonFrame_Bailed_JS)
        type_ = IonFrame_JS;
    returnAddressToFp_ = current()->returnAddress();
    current_ = prev;
    return *this;
}

uintptr_t *
IonFrameIterator::spillBase() const
{
    // Get the base address to where safepoint registers are spilled.
    // Out-of-line calls do not unwind the extra padding space used to
    // aggregate bailout tables, so we use frameSize instead of frameLocals,
    // which would only account for local stack slots.
    return reinterpret_cast<uintptr_t *>(fp() - ionScript()->frameSize());
}

MachineState
IonFrameIterator::machineState() const
{
    SafepointReader reader(ionScript(), safepoint());
    uintptr_t *spill = spillBase();

    // see CodeGeneratorShared::saveLive, we are only copying GPRs for now, FPUs
    // are stored after but are not saved in the safepoint.  This means that we
    // are unable to restore any FPUs registers from an OOL VM call.  This can
    // cause some trouble for f.arguments.
    MachineState machine;
    for (GeneralRegisterIterator iter(reader.allSpills()); iter.more(); iter++)
        machine.setRegisterLocation(*iter, --spill);

    return machine;
}

static void
CloseLiveIterator(JSContext *cx, const InlineFrameIterator &frame, uint32 localSlot)
{
    SnapshotIterator si = frame.snapshotIterator();

    // Skip stack slots until we reach the iterator object.
    uint32 base = CountArgSlots(frame.maybeCallee()) + frame.script()->nfixed;
    uint32 skipSlots = base + localSlot - 1;

    for (unsigned i = 0; i < skipSlots; i++)
        si.skip();

    Value v = si.read();
    JSObject *obj = &v.toObject();

    if (cx->isExceptionPending())
        UnwindIteratorForException(cx, obj);
    else
        UnwindIteratorForUncatchableException(cx, obj);
}

static void
CloseLiveIterators(JSContext *cx, const InlineFrameIterator &frame)
{
    JSScript *script = frame.script();
    jsbytecode *pc = frame.pc();

    if (!script->hasTrynotes())
        return;

    JSTryNote *tn = script->trynotes()->vector;
    JSTryNote *tnEnd = tn + script->trynotes()->length;

    uint32 pcOffset = uint32(pc - script->main());
    for (; tn != tnEnd; ++tn) {
        if (pcOffset < tn->start)
            continue;
        if (pcOffset >= tn->start + tn->length)
            continue;

        if (tn->kind != JSTRY_ITER)
            continue;

        JS_ASSERT(JSOp(*(script->main() + tn->start + tn->length)) == JSOP_ENDITER);
        JS_ASSERT(tn->stackDepth > 0);

        uint32 localSlot = tn->stackDepth;
        CloseLiveIterator(cx, frame, localSlot);
    }
}

void
ion::HandleException(ResumeFromException *rfe)
{
    JSContext *cx = GetIonContext()->cx;

    IonSpew(IonSpew_Invalidate, "handling exception");

    IonFrameIterator iter(cx->runtime->ionTop);
    while (!iter.isEntry()) {
        if (iter.isScripted()) {
            // Search each inlined frame for live iterator objects, and close
            // them.
            InlineFrameIterator frames(&iter);
            for (;;) {
                CloseLiveIterators(cx, frames);
                if (!frames.more())
                    break;
                ++frames;
            }

            IonScript *ionScript;
            if (iter.checkInvalidation(&ionScript))
                ionScript->decref(cx->runtime->defaultFreeOp());
        }

        ++iter;
    }

    // Clear any Ion return override that's been set.
    // This may happen if a callVM function causes an invalidation (setting the
    // override), and then fails, bypassing the bailout handlers that would
    // otherwise clear the return override.
    if (cx->runtime->hasIonReturnOverride())
        cx->runtime->takeIonReturnOverride();

    rfe->stackPointer = iter.fp();
}

IonActivationIterator::IonActivationIterator(JSContext *cx)
  : top_(cx->runtime->ionTop),
    activation_(cx->runtime->ionActivation)
{
}

IonActivationIterator::IonActivationIterator(JSRuntime *rt)
  : top_(rt->ionTop),
    activation_(rt->ionActivation)
{
}

IonActivationIterator &
IonActivationIterator::operator++()
{
    JS_ASSERT(activation_);
    top_ = activation_->prevIonTop();
    activation_ = activation_->prev();
    return *this;
}

bool
IonActivationIterator::more() const
{
    return !!activation_;
}

static void
MarkCalleeToken(JSTracer *trc, CalleeToken token)
{
    switch (GetCalleeTokenTag(token)) {
      case CalleeToken_Function:
      {
        JSFunction *fun = CalleeTokenToFunction(token);
        MarkObjectRoot(trc, &fun, "ion-callee");
        JS_ASSERT(fun == CalleeTokenToFunction(token));
        break;
      }
      case CalleeToken_Script:
      {
        JSScript *script = CalleeTokenToScript(token);
        MarkScriptRoot(trc, &script, "ion-entry");
        JS_ASSERT(script == CalleeTokenToScript(token));
        break;
      }
      default:
        JS_NOT_REACHED("unknown callee token type");
    }
}

static inline uintptr_t
ReadAllocation(const IonFrameIterator &frame, const LAllocation *a)
{
    if (a->isGeneralReg()) {
        Register reg = a->toGeneralReg()->reg();
        return frame.machineState().read(reg);
    }
    if (a->isStackSlot()) {
        uint32 slot = a->toStackSlot()->slot();
        return *frame.jsFrame()->slotRef(slot);
    }
    uint32 index = a->toArgument()->index();
    uint8 *argv = reinterpret_cast<uint8 *>(frame.jsFrame()->argv());
    return *reinterpret_cast<uintptr_t *>(argv + index);
}

static void
MarkIonJSFrame(JSTracer *trc, const IonFrameIterator &frame)
{
    IonJSFrameLayout *layout = (IonJSFrameLayout *)frame.fp();

    MarkCalleeToken(trc, layout->calleeToken());

    IonScript *ionScript;
    if (frame.checkInvalidation(&ionScript)) {
        // This frame has been invalidated, meaning that its IonScript is no
        // longer reachable through the callee token (JSFunction/JSScript->ion
        // is now NULL or recompiled). Manually trace it here.
        IonScript::Trace(trc, ionScript);
    } else if (CalleeTokenIsFunction(layout->calleeToken())) {
        ionScript = CalleeTokenToFunction(layout->calleeToken())->script()->ion;
    } else {
        ionScript = CalleeTokenToScript(layout->calleeToken())->ion;
    }

    if (CalleeTokenIsFunction(layout->calleeToken())) {
        // (NBP) We do not need to mark formal arguments since they are covered
        // by the safepoint.
        size_t nargs = frame.numActualArgs();

        // Trace function arguments. Note + 1 for thisv.
        Value *argv = layout->argv();
        for (size_t i = 0; i < nargs + 1; i++)
            gc::MarkValueRoot(trc, &argv[i], "ion-argv");
    }

    const SafepointIndex *si = ionScript->getSafepointIndex(frame.returnAddressToFp());

    SafepointReader safepoint(ionScript, si);

    // Scan through slots which contain pointers (or on punboxing systems,
    // actual values).
    uint32 slot;
    while (safepoint.getGcSlot(&slot)) {
        uintptr_t *ref = layout->slotRef(slot);
        gc::MarkThingOrValueRoot(trc, ref, "ion-gc-slot");
    }

    while (safepoint.getValueSlot(&slot)) {
        Value *v = (Value *)layout->slotRef(slot);
        gc::MarkValueRoot(trc, v, "ion-gc-slot");
    }

    uintptr_t *spill = frame.spillBase();
    GeneralRegisterSet gcRegs = safepoint.gcSpills();
    for (GeneralRegisterIterator iter(safepoint.allSpills()); iter.more(); iter++) {
        --spill;
        if (gcRegs.has(*iter))
            gc::MarkThingOrValueRoot(trc, spill, "ion-gc-spill");
    }

#ifdef JS_NUNBOX32
    LAllocation type, payload;
    while (safepoint.getNunboxSlot(&type, &payload)) {
        jsval_layout layout;
        layout.s.tag = (JSValueTag)ReadAllocation(frame, &type);
        layout.s.payload.uintptr = ReadAllocation(frame, &payload);

        Value v = IMPL_TO_JSVAL(layout);
        gc::MarkValueRoot(trc, &v, "ion-torn-value");
        JS_ASSERT(v == IMPL_TO_JSVAL(layout));
    }
#endif
}

static void
MarkIonExitFrame(JSTracer *trc, const IonFrameIterator &frame)
{
    IonExitFooterFrame *footer = frame.exitFrame()->footer();

    // Mark the code of the code handling the exit path.  This is needed because
    // invalidated script are no longer marked because data are erased by the
    // invalidation and relocation data are no longer reliable.  So the VM
    // wrapper or the invalidation code may be GC if no IonCode keep reference
    // on them.
    JS_ASSERT(uintptr_t(footer->ionCode()) != uintptr_t(-1));

    // This correspond to the case where we have build a fake exit frame in
    // CodeGenerator.cpp which handle the case of a native function call. We
    // need to mark the argument vector of the function call.
    if (frame.isNative()) {
        IonNativeExitFrameLayout *native = frame.exitFrame()->nativeExit();
        size_t len = native->argc() + 2;
        Value *vp = native->vp();
        gc::MarkValueRootRange(trc, len, vp, "ion-native-args");
        return;
    }

    MarkIonCodeRoot(trc, footer->addressOfIonCode(), "ion-exit-code");

    const VMFunction *f = footer->function();
    if (f == NULL || f->explicitArgs == 0)
        return;

    // Mark arguments of the VM wrapper.
    uint8 *argBase = frame.exitFrame()->argBase();
    for (uint32 explicitArg = 0; explicitArg < f->explicitArgs; explicitArg++) {
        switch (f->argRootType(explicitArg)) {
          case VMFunction::RootNone:
            break;
          case VMFunction::RootObject: {
            // Sometimes we can bake in HandleObjects to NULL.
            JSObject **pobj = reinterpret_cast<JSObject **>(argBase);
            if (*pobj)
                gc::MarkObjectRoot(trc, pobj, "ion-vm-args");
            break;
          }
          case VMFunction::RootString:
          case VMFunction::RootPropertyName:
            gc::MarkStringRoot(trc, reinterpret_cast<JSString**>(argBase), "ion-vm-args");
            break;
          case VMFunction::RootFunction:
            gc::MarkObjectRoot(trc, reinterpret_cast<JSFunction**>(argBase), "ion-vm-args");
            break;
          case VMFunction::RootValue:
            gc::MarkValueRoot(trc, reinterpret_cast<Value*>(argBase), "ion-vm-args");
            break;
          case VMFunction::RootCell:
            gc::MarkThingOrValueRoot(trc, reinterpret_cast<uintptr_t *>(argBase), "ion-vm-args");
            break;
        }

        switch (f->argProperties(explicitArg)) {
          case VMFunction::WordByValue:
          case VMFunction::WordByRef:
            argBase += sizeof(void *);
            break;
          case VMFunction::DoubleByValue:
          case VMFunction::DoubleByRef:
            argBase += 2 * sizeof(void *);
            break;
        }
    }
}

static void
MarkIonActivation(JSTracer *trc, const IonActivationIterator &activations)
{
    for (IonFrameIterator frames(activations); !frames.done(); ++frames) {
        switch (frames.type()) {
          case IonFrame_Exit:
            MarkIonExitFrame(trc, frames);
            break;
          case IonFrame_JS:
            MarkIonJSFrame(trc, frames);
            break;
          case IonFrame_Bailed_JS:
            JS_NOT_REACHED("invalid");
            break;
          case IonFrame_Rectifier:
          case IonFrame_Bailed_Rectifier: {
            IonCompartment *ionCompartment = activations.activation()->compartment()->ionCompartment();
            MarkIonCodeRoot(trc, ionCompartment->getArgumentsRectifierAddr(), "Arguments Rectifier");
            break;
          }
          case IonFrame_Osr:
            // The callee token will be marked by the callee JS frame;
            // otherwise, it does not need to be marked, since the frame is
            // dead.
            break;
          default:
            JS_NOT_REACHED("unexpected frame type");
            break;
        }
    }
}

void
ion::MarkIonActivations(JSRuntime *rt, JSTracer *trc)
{
    for (IonActivationIterator activations(rt); activations.more(); ++activations)
        MarkIonActivation(trc, activations);
}

void
ion::MarkIonCompilerRoots(JSTracer *trc)
{
    JSRuntime *rt = trc->runtime;
    for (CompilerRootNode *root = rt->ionCompilerRootList; root != NULL; root = root->next)
        gc::MarkGCThingRoot(trc, root->address(), "ion-compiler-root");
}

void
ion::GetPcScript(JSContext *cx, JSScript **scriptRes, jsbytecode **pcRes)
{
    JS_ASSERT(cx->fp()->runningInIon());
    IonSpew(IonSpew_Snapshots, "Recover PC & Script from the last frame.");

    // Recover the innermost inlined frame.
    IonFrameIterator it(cx->runtime->ionTop);
    ++it;
    InlineFrameIterator ifi(&it);

    // Set the result.
    *scriptRes = ifi.script();
    if (pcRes)
        *pcRes = ifi.pc();
}

void
OsiIndex::fixUpOffset(MacroAssembler &masm)
{
    callPointDisplacement_ = masm.actualOffset(callPointDisplacement_);
}

uint32
OsiIndex::returnPointDisplacement() const
{
    // In general, pointer arithmetic on code is bad, but in this case,
    // getting the return address from a call instruction, stepping over pools
    // would be wrong.
    return callPointDisplacement_ + Assembler::patchWrite_NearCallSize();
}

SnapshotIterator::SnapshotIterator(IonScript *ionScript, SnapshotOffset snapshotOffset,
                                   IonJSFrameLayout *fp, const MachineState &machine)
  : SnapshotReader(ionScript->snapshots() + snapshotOffset,
                   ionScript->snapshots() + ionScript->snapshotsSize()),
    fp_(fp),
    machine_(machine),
    ionScript_(ionScript)
{
    JS_ASSERT(snapshotOffset < ionScript->snapshotsSize());
}

SnapshotIterator::SnapshotIterator(const IonFrameIterator &iter)
  : SnapshotReader(iter.ionScript()->snapshots() + iter.osiIndex()->snapshotOffset(),
                   iter.ionScript()->snapshots() + iter.ionScript()->snapshotsSize()),
    fp_(iter.jsFrame()),
    machine_(iter.machineState()),
    ionScript_(iter.ionScript())
{
}

SnapshotIterator::SnapshotIterator()
  : SnapshotReader(NULL, NULL),
    fp_(NULL),
    ionScript_(NULL)
{
}

bool
SnapshotIterator::hasLocation(const SnapshotReader::Location &loc)
{
    return loc.isStackSlot() || machine_.has(loc.reg());
}

uintptr_t
SnapshotIterator::fromLocation(const SnapshotReader::Location &loc)
{
    if (loc.isStackSlot())
        return ReadFrameSlot(fp_, loc.stackSlot());
    return machine_.read(loc.reg());
}

Value
SnapshotIterator::FromTypedPayload(JSValueType type, uintptr_t payload)
{
    switch (type) {
      case JSVAL_TYPE_INT32:
        return Int32Value(payload);
      case JSVAL_TYPE_BOOLEAN:
        return BooleanValue(!!payload);
      case JSVAL_TYPE_STRING:
        return StringValue(reinterpret_cast<JSString *>(payload));
      case JSVAL_TYPE_OBJECT:
        return ObjectValue(*reinterpret_cast<JSObject *>(payload));
      case JSVAL_TYPE_MAGIC:
        // This is an argument object.
        if (payload == 0)
            return MagicValue(JS_OPTIMIZED_ARGUMENTS);
        return ObjectValue(*reinterpret_cast<JSObject *>(payload));
      default:
        JS_NOT_REACHED("unexpected type - needs payload");
        return UndefinedValue();
    }
}

bool
SnapshotIterator::slotReadable(const Slot &slot)
{
    switch (slot.mode()) {
      case SnapshotReader::DOUBLE_REG:
        return machine_.has(slot.floatReg());

      case SnapshotReader::TYPED_REG:
        return machine_.has(slot.reg());

      case SnapshotReader::UNTYPED:
#if defined(JS_NUNBOX32)
          return hasLocation(slot.type()) && hasLocation(slot.payload());
#elif defined(JS_PUNBOX64)
          return hasLocation(slot.value());
#endif

      default:
        return true;
    }
}

Value
SnapshotIterator::slotValue(const Slot &slot)
{
    switch (slot.mode()) {
      case SnapshotReader::DOUBLE_REG:
        return DoubleValue(machine_.read(slot.floatReg()));

      case SnapshotReader::TYPED_REG:
        return FromTypedPayload(slot.knownType(), machine_.read(slot.reg()));

      case SnapshotReader::TYPED_STACK:
      {
        JSValueType type = slot.knownType();
        if (type == JSVAL_TYPE_DOUBLE)
            return DoubleValue(ReadFrameDoubleSlot(fp_, slot.stackSlot()));
        return FromTypedPayload(type, ReadFrameSlot(fp_, slot.stackSlot()));
      }

      case SnapshotReader::UNTYPED:
      {
          jsval_layout layout;
#if defined(JS_NUNBOX32)
          layout.s.tag = (JSValueTag)fromLocation(slot.type());
          layout.s.payload.word = fromLocation(slot.payload());
#elif defined(JS_PUNBOX64)
          layout.asBits = fromLocation(slot.value());
#endif
          return IMPL_TO_JSVAL(layout);
      }

      case SnapshotReader::JS_UNDEFINED:
        return UndefinedValue();

      case SnapshotReader::JS_NULL:
        return NullValue();

      case SnapshotReader::JS_INT32:
        return Int32Value(slot.int32Value());

      case SnapshotReader::CONSTANT:
        return ionScript_->getConstant(slot.constantIndex());

      default:
        JS_NOT_REACHED("huh?");
        return UndefinedValue();
    }
}

IonScript *
IonFrameIterator::ionScript() const
{
    JS_ASSERT(type() == IonFrame_JS);

    IonScript *ionScript;
    if (checkInvalidation(&ionScript))
        return ionScript;
    return script()->ionScript();
}

const SafepointIndex *
IonFrameIterator::safepoint() const
{
    if (!cachedSafepointIndex_)
        cachedSafepointIndex_ = ionScript()->getSafepointIndex(returnAddressToFp());
    return cachedSafepointIndex_;
}

const OsiIndex *
IonFrameIterator::osiIndex() const
{
    SafepointReader reader(ionScript(), safepoint());
    return ionScript()->getOsiIndex(reader.osiReturnPointOffset());
}

InlineFrameIterator::InlineFrameIterator(const IonFrameIterator *iter)
  : frame_(iter),
    framesRead_(0),
    callee_(NULL),
    script_(NULL)
{
    if (iter) {
        start_ = SnapshotIterator(*iter);
        findNextFrame();
    }
}

void
InlineFrameIterator::findNextFrame()
{
    JS_ASSERT(more());

    si_ = start_;

    // Read the initial frame.
    callee_ = frame_->maybeCallee();
    script_ = frame_->script();
    pc_ = script_->code + si_.pcOffset();
#ifdef DEBUG
    numActualArgs_ = 0xbad;
#endif

    // This unfortunately is O(n*m), because we must skip over outer frames
    // before reading inner ones.
    unsigned remaining = start_.frameCount() - framesRead_ - 1;
    for (unsigned i = 0; i < remaining; i++) {
        JS_ASSERT(js_CodeSpec[*pc_].format & JOF_INVOKE);

        // Recover the number of actual arguments from the script.
        numActualArgs_ = GET_ARGC(pc_);

        // Skip over non-argument slots, as well as |this|.
        unsigned skipCount = (si_.slots() - 1) - numActualArgs_ - 1;
        for (unsigned j = 0; j < skipCount; j++)
            si_.skip();

        Value funval = si_.read();

        // Skip extra slots.
        while (si_.moreSlots())
            si_.skip();

        si_.nextFrame();

        callee_ = funval.toObject().toFunction();
        script_ = callee_->script();
        pc_ = script_->code + si_.pcOffset();
    }

    framesRead_++;
}

InlineFrameIterator
InlineFrameIterator::operator++()
{
    InlineFrameIterator iter(*this);
    findNextFrame();
    return iter;
}

bool
InlineFrameIterator::isFunctionFrame() const
{
    return !!callee_;
}

MachineState
MachineState::FromBailout(uintptr_t regs[Registers::Total],
                          double fpregs[FloatRegisters::Total])
{
    MachineState machine;

    for (unsigned i = 0; i < Registers::Total; i++)
        machine.setRegisterLocation(Register::FromCode(i), &regs[i]);
    for (unsigned i = 0; i < FloatRegisters::Total; i++)
        machine.setRegisterLocation(FloatRegister::FromCode(i), &fpregs[i]);

    return machine;
}

bool
InlineFrameIterator::isConstructing() const
{
    // Skip the current frame and look at the caller's.
    if (more()) {
        InlineFrameIterator parent(*this);
        ++parent;

        // Inlined Getters and Setters are never constructing.
        if (IsGetterPC(parent.pc()) || IsSetterPC(parent.pc()))
            return false;

        // In the case of a JS frame, look up the pc from the snapshot.
        JS_ASSERT(js_CodeSpec[*parent.pc()].format & JOF_INVOKE);

        return (JSOp)*parent.pc() == JSOP_NEW;
    }

    return frame_->isConstructing();
}

bool
IonFrameIterator::isConstructing() const
{
    IonFrameIterator parent(*this);

    // Skip the current frame and look at the caller's.
    do {
        ++parent;
    } while (!parent.done() && !parent.isScripted());

    if (parent.isScripted()) {
        // In the case of a JS frame, look up the pc from the snapshot.
        InlineFrameIterator inlinedParent(&parent);

        //Inlined Getters and Setters are never constructing.
        if (IsGetterPC(inlinedParent.pc()) || IsSetterPC(inlinedParent.pc()))
            return false;

        JS_ASSERT(js_CodeSpec[*inlinedParent.pc()].format & JOF_INVOKE);

        return (JSOp)*inlinedParent.pc() == JSOP_NEW;
    }

    JS_ASSERT(parent.done());
    return activation_->entryfp()->isConstructing();
}

JSObject *
InlineFrameIterator::thisObject() const
{
    // JS_ASSERT(isConstructing(...));
    SnapshotIterator s(si_);

    // scopeChain
    s.skip();

    // In strict modes, |this| may not be an object and thus may not be
    // readable which can either segv in read or trigger the assertion.
    Value v = s.read();
    JS_ASSERT(v.isObject());
    return &v.toObject();
}

unsigned
InlineFrameIterator::numActualArgs() const
{
    // The number of actual arguments of inline frames is recovered by the
    // iteration process. It is recovered from the bytecode because this
    // property still hold since the for inlined frames. This property does not
    // hold for the parent frame because it can have optimize a call to
    // js_fun_call or js_fun_apply.
    if (more())
        return numActualArgs_;

    return frame_->numActualArgs();
}

unsigned
IonFrameIterator::numActualArgs() const
{
    if (isScripted())
        return jsFrame()->numActualArgs();

    JS_ASSERT(isNative());
    return exitFrame()->nativeExit()->argc();
}

void
SnapshotIterator::warnUnreadableSlot()
{
    fprintf(stderr, "Warning! Tried to access unreadable IonMonkey slot (possible f.arguments).\n");
}

void
IonFrameIterator::dump() const
{
    switch (type_) {
      case IonFrame_Entry:
        fprintf(stderr, " Entry frame\n");
        fprintf(stderr, "  Frame size: %u\n", unsigned(current()->prevFrameLocalSize()));
        break;
      case IonFrame_JS:
      {
        InlineFrameIterator frames(this);
        for (;;) {
            frames.dump();
            if (!frames.more())
                break;
            ++frames;
        }
        break;
      }
      case IonFrame_Rectifier:
      case IonFrame_Bailed_Rectifier:
        fprintf(stderr, " Rectifier frame\n");
        fprintf(stderr, "  Frame size: %u\n", unsigned(current()->prevFrameLocalSize()));
        break;
      case IonFrame_Bailed_JS:
        fprintf(stderr, "Warning! Bailed JS frames are not observable.\n");
        break;
      case IonFrame_Exit:
        break;
      case IonFrame_Osr:
        fprintf(stderr, "Warning! OSR frame are not defined yet.\n");
        break;
    };
    fputc('\n', stderr);
}

struct DumpOp {
    DumpOp(unsigned int i) : i_(i) {}

    unsigned int i_;
    void operator()(const Value& v) {
        fprintf(stderr, "  actual (arg %d): ", i_);
#ifdef DEBUG
        js_DumpValue(v);
#else
        fputc('?\n', stderr);
#endif
        i_++;
    }
};

void
InlineFrameIterator::dump() const
{
    if (more())
        fprintf(stderr, " JS frame (inlined)\n");
    else
        fprintf(stderr, " JS frame\n");

    bool isFunction = false;
    if (isFunctionFrame()) {
        isFunction = true;
        fprintf(stderr, "  callee fun: ");
#ifdef DEBUG
        js_DumpObject(callee());
#else
        fputc('?\n', stderr);
#endif
    } else {
        fprintf(stderr, "  global frame, no callee\n");
    }

    fprintf(stderr, "  file %s line %u\n",
            script()->filename, (unsigned) script()->lineno);

    fprintf(stderr, "  script = %p, pc = %p\n", (void*) script(), pc());
    fprintf(stderr, "  current op: %s\n", js_CodeName[*pc()]);

    if (!more()) {
        numActualArgs();
    }

    SnapshotIterator si = snapshotIterator();
    fprintf(stderr, "  slots: %u\n", si.slots() - 1);
    for (unsigned i = 0; i < si.slots() - 1; i++) {
        if (isFunction) {
            if (i == 0)
                fprintf(stderr, "  scope chain: ");
            else if (i == 1)
                fprintf(stderr, "  this: ");
            else if (i - 2 < callee()->nargs)
                fprintf(stderr, "  formal (arg %d): ", i - 2);
            else {
                if (i - 2 == callee()->nargs && numActualArgs() > callee()->nargs) {
                    DumpOp d(callee()->nargs);
                    forEachCanonicalActualArg(d, d.i_, numActualArgs());
                }

                fprintf(stderr, "  slot %d: ", i - 2 - callee()->nargs);
            }
        } else
            fprintf(stderr, "  slot %u: ", i);
#ifdef DEBUG
        js_DumpValue(si.maybeRead());
#else
        fputc('?\n', stderr);
#endif
    }

    fputc('\n', stderr);
}
