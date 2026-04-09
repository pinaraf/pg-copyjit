

// No trampoline on amd64
#define TRAMPOLINE_SIZE 0


static void apply_jump(CodeGen *codeGen, size_t offset, intptr_t target, const struct Patch *patch)
{
    // A LOT OF FUN !
    // target is an address we need to jump to. we are playing with code with IP = offset+patch->offset
    if (DEBUG_GEN)
        elog(WARNING, "Asked to jump to %p, we are patching at %p", target, (intptr_t) codeGen->code.as_void + offset + patch->offset);
    int64_t relative_jump = target - ((intptr_t) codeGen->code.as_void + offset + patch->offset);
    // Note : one could build short jumps, but not sure it's worth the effort
    // I assert we have no insane jump, but... meh, should implement a check
    relative_jump -= 5;	// remove size of jump from offset
    int32_t near_jump = (int32_t) relative_jump;
    codeGen->code.as_char[offset + patch->offset] = 0xE9;
    memcpy(codeGen->code.as_void + offset + patch->offset + 1, &near_jump, 4);
}

static void apply_patch_with_target (CodeGen *codeGen, size_t offset, intptr_t target, const struct Patch *patch)
{
    if (DEBUG_GEN)
        elog(WARNING, "Applying a patch at offset %i+%i, target %p, kind %i", offset, patch->offset, target, patch->relkind);
    switch (patch->relkind) {
        case RELKIND_R_X86_64_64:
            memcpy(codeGen->code.as_void + offset + patch->offset, &target, 8);
            break;
        case RELKIND_REJUMP: // Reminder: this is an artificial one we created
            apply_jump(codeGen, offset, target, patch);
            break;
        default:
            elog(ERROR, "Unsupported relkind");
            break;
    }
}
