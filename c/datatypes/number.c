#include "number.h"
#include "../vm.h"

static Value toStringNumber(VM *vm, int argCount, Value *args) {
    if (argCount != 0) {
        runtimeError(vm, "toString() takes no arguments (%d given)", argCount);
        return EMPTY_VAL;
    }

    double number = AS_NUMBER(args[0]);
    int numberStringLength = snprintf(NULL, 0, "%.15g", number) + 1;
    char numberString[numberStringLength];
    snprintf(numberString, numberStringLength, "%.15g", number);
    return OBJ_VAL(copyString(vm, numberString, numberStringLength - 1));
}

void declareNumberMethods(VM *vm) {
    defineNative(vm, &vm->numberMethods, "toString", toStringNumber);
}
