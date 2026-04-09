
#define TRAMPOLINE_SIZE 16

static void build_aarch64_trampoline(uint32_t *code, intptr_t target)
{
    // Unlike x86, arm has a fixed instruction width.
    // When it switched to 64 bits, instead of killing code density and thus performance,
    // it stayed on 32 bits instruction width. This makes jumping to arbitrary address harder.
    // We create a small "trampoline", containing the following code:
    // ldr x8, 8        <== load data from current IP+8 into x8
    // br x8            <== branch to the data loaded previously
    // XXXX             <== first part of target
    // YYYY             <== second part of target
    // where XXXX and YYYY are the two 32-bits parts of the 64 bits target.
    // x8 is a scratch register, we are free to trash it.
    // Note that code is a uint32_t here to use this fixed-instruction property...
    code[0] = 0x58000048;
    code[1] = 0xD61F0100;
    code[2] = target & 0xffffffff;
    code[3] = target >> 32;
}

static void apply_arm64_x26 (CodeGen *codeGen, size_t u32offset, intptr_t target)
{
    intptr_t current_address = &(codeGen->code.as_u32[u32offset]);
    uint32_t delta = (target - current_address) / 4;
    if ((delta < (1 << 26)) && (delta > -(1 << 26))) {
        if (DEBUG_GEN) {
            elog(WARNING, "*** Jump does not require a trampoline***");
            elog(WARNING, "==> Delta = %p for %p - %p", delta, target, current_address);
        }
        else {
            if (DEBUG_GEN)
                elog(WARNING, "Asked to create a trampoline targeting %p for offset %p", target, u32offset);
            uint32_t *trampoline_address = codeGen->code.as_u32 + codeGen->code_size / 4;	// Target the beginning of trampoline area, after code
            int t;
            for (t = 0 ; t < codeGen->trampoline_count ; t++) {
                trampoline_address += (TRAMPOLINE_SIZE / 4);
                if (codeGen->trampoline_targets[t] == target)
                    break;
            }
            if (DEBUG_GEN)
                elog(WARNING, "=> Going to use trampoline %x, at %p", t, trampoline_address);
            if (t == codeGen->trampoline_count) {
                // The target has not yet been 'trampolined', let's do it
                build_aarch64_trampoline(trampoline_address, target);
                codeGen->trampoline_targets[t] = target;
                codeGen->trampoline_count++;
            }
            // Now we can code a 26bits delta using the offset between codeGen->code+u32offset and trampoline_address
            delta = (((intptr_t) trampoline_address) - current_address) /4;
            if (DEBUG_GEN)
                elog(WARNING, "=> Delta = %p for %p - %p", delta, trampoline_address, current_address);
            if (delta > (1 << 26) || delta < -(1 << 26))
                elog(WARNING, "Computed delta, %p, from %p to %p, is far too big", delta, current_address, trampoline_address);
        }
        // Force instruction target bits to 0, for safety
        codeGen->code.as_u32[u32offset] &= 0xFC000000;
        // Now encode the delta in there
        codeGen->code.as_u32[u32offset] |= (delta & ~0xFC000000);
    }
}

static void apply_patch_with_target (CodeGen *codeGen, size_t offset, intptr_t target, const struct Patch *patch)
{
    size_t u32offset = (offset + patch->offset) / 4;
    uint32_t value;
    if (DEBUG_GEN)
        elog(WARNING, "Applying a patch at offset %i+%i, target %p, kind %i", offset, patch->offset, target, patch->relkind);
    switch (patch->relkind) {
        case RELKIND_R_AARCH64_MOVW_UABS_G0_NC:
            value = target & 0xFFFF;
            if (DEBUG_GEN)
                elog(WARNING, "Patching 0x%08x with value %p (moved to %p)", codeGen->code.as_u32[u32offset], value, value << 5);
        codeGen->code.as_u32[u32offset] = (codeGen->code.as_u32[u32offset] | (value << 5));
        if (DEBUG_GEN)
            elog(WARNING, "Patched 0x%08x with value %p (moved to %p)", codeGen->code.as_u32[u32offset], value, value << 5);
        break;
        case RELKIND_R_AARCH64_MOVW_UABS_G1_NC:
            value = (target & 0xFFFF0000) >> 16;
            if (DEBUG_GEN)
                elog(WARNING, "Patching 0x%08x with value %p (moved to %p)", codeGen->code.as_u32[u32offset], value, value << 5);
        codeGen->code.as_u32[u32offset] = (codeGen->code.as_u32[u32offset] | (value << 5));
        if (DEBUG_GEN)
            elog(WARNING, "Patched 0x%08x with value %p (moved to %p)", codeGen->code.as_u32[u32offset], value, value << 5);
        break;
        case RELKIND_R_AARCH64_MOVW_UABS_G2_NC:
            value = (target & 0xFFFF00000000) >> 32;
            if (DEBUG_GEN)
                elog(WARNING, "Patching 0x%08x with value %p (moved to %p)", codeGen->code.as_u32[u32offset], value, value << 5);
        codeGen->code.as_u32[u32offset] = (codeGen->code.as_u32[u32offset] | (value << 5));
        if (DEBUG_GEN)
            elog(WARNING, "Patched 0x%08x with value %p (moved to %p)", codeGen->code.as_u32[u32offset], value, value << 5);
        break;
        case RELKIND_R_AARCH64_MOVW_UABS_G3:
            value = (target & 0xFFFF000000000000) >> 48;
            if (DEBUG_GEN)
                elog(WARNING, "Patching 0x%08x with value %p (moved to %p)", codeGen->code.as_u32[u32offset], value, value << 5);
        codeGen->code.as_u32[u32offset] = (codeGen->code.as_u32[u32offset] | (value << 5));
        if (DEBUG_GEN)
            elog(WARNING, "Patched 0x%08x with value %p (moved to %p)", codeGen->code.as_u32[u32offset], value, value << 5);
        break;
        // These two require trampolines
        case RELKIND_R_AARCH64_JUMP26:
        case RELKIND_R_AARCH64_CALL26:
            apply_arm64_x26(codeGen, u32offset, target);
            break;
        default:
            elog(ERROR, "Unsupported relkind");
            break;
    }
}
