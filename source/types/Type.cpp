//------------------------------------------------------------------------------
// Type.cpp
// Base class for all expression types
//
// File is under the MIT license; see LICENSE for details
//------------------------------------------------------------------------------
#include "slang/types/Type.h"

#include "slang/binding/Bitstream.h"
#include "slang/compilation/Compilation.h"
#include "slang/diagnostics/LookupDiags.h"
#include "slang/diagnostics/TypesDiags.h"
#include "slang/parsing/LexerFacts.h"
#include "slang/symbols/ASTSerializer.h"
#include "slang/symbols/ASTVisitor.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/types/AllTypes.h"
#include "slang/types/TypePrinter.h"
#include "slang/util/StackContainer.h"

namespace slang {

namespace {

struct GetDefaultVisitor {
    template<typename T>
    using getDefault_t = decltype(std::declval<T>().getDefaultValueImpl());

    template<typename T>
    ConstantValue visit([[maybe_unused]] const T& type) {
        if constexpr (is_detected_v<getDefault_t, T>) {
            return type.getDefaultValueImpl();
        }
        else {
            THROW_UNREACHABLE;
        }
    }
};

bool isSameEnum(const EnumType& le, const EnumType& re) {
    auto ls = le.getParentScope();
    auto rs = re.getParentScope();
    if (!ls || !rs)
        return false;

    if (ls->asSymbol().kind != SymbolKind::CompilationUnit)
        return false;

    if (rs->asSymbol().kind != SymbolKind::CompilationUnit)
        return false;

    if (!le.baseType.isMatching(re.baseType))
        return false;

    auto rr = re.values();
    auto rit = rr.begin();
    for (auto& value : le.values()) {
        if (rit == rr.end())
            return false;

        if (value.name != rit->name)
            return false;

        auto& lv = value.getValue();
        auto& rv = rit->getValue();
        if (lv.bad() || rv.bad())
            return false;

        if (lv.integer() != rv.integer())
            return false;

        rit++;
    }

    return rit == rr.end();
}

} // namespace

bitwidth_t Type::getBitWidth() const {
    const Type& ct = getCanonicalType();
    if (ct.isIntegral())
        return ct.as<IntegralType>().bitWidth;

    if (ct.isFloating()) {
        switch (ct.as<FloatingType>().floatKind) {
            case FloatingType::Real:
                return 64;
            case FloatingType::RealTime:
                return 64;
            case FloatingType::ShortReal:
                return 32;
            default:
                THROW_UNREACHABLE;
        }
    }
    return 0;
}

size_t Type::bitstreamWidth() const {
    size_t width = getBitWidth();
    if (width > 0)
        return width;

    // TODO: check for overflow
    if (isUnpackedArray()) {
        auto& ct = getCanonicalType();
        if (ct.kind != SymbolKind::FixedSizeUnpackedArrayType)
            return 0;

        auto& fsa = ct.as<FixedSizeUnpackedArrayType>();
        return fsa.elementType.bitstreamWidth() * fsa.range.width();
    }

    if (isUnpackedStruct()) {
        auto& us = getCanonicalType().as<UnpackedStructType>();
        for (const auto& field : us.membersOfType<FieldSymbol>())
            width += field.getType().bitstreamWidth();
    }

    if (isUnpackedUnion()) {
        // Unpacked unions are not bitstream types but we support
        // getting a bit width out of them anyway.
        auto& us = getCanonicalType().as<UnpackedUnionType>();
        for (const auto& field : us.membersOfType<FieldSymbol>())
            width = std::max(width, field.getType().bitstreamWidth());
    }

    if (isClass()) {
        auto& ct = getCanonicalType().as<ClassType>();
        if (ct.isInterface)
            return 0;

        for (auto& prop : ct.membersOfType<ClassPropertySymbol>())
            width += prop.getType().bitstreamWidth();
    }

    return width;
}

bool Type::isSigned() const {
    const Type& ct = getCanonicalType();
    return ct.isIntegral() && ct.as<IntegralType>().isSigned;
}

bool Type::isFourState() const {
    const Type& ct = getCanonicalType();
    if (ct.isIntegral())
        return ct.as<IntegralType>().isFourState;

    if (ct.isArray())
        return ct.getArrayElementType()->isFourState();

    switch (ct.kind) {
        case SymbolKind::UnpackedStructType: {
            auto& us = ct.as<UnpackedStructType>();
            for (auto& field : us.membersOfType<FieldSymbol>()) {
                if (field.getType().isFourState())
                    return true;
            }
            return false;
        }
        case SymbolKind::UnpackedUnionType: {
            auto& us = ct.as<UnpackedUnionType>();
            for (auto& field : us.membersOfType<FieldSymbol>()) {
                if (field.getType().isFourState())
                    return true;
            }
            return false;
        }
        default:
            return false;
    }
}

bool Type::isIntegral() const {
    const Type& ct = getCanonicalType();
    return IntegralType::isKind(ct.kind);
}

bool Type::isAggregate() const {
    switch (getCanonicalType().kind) {
        case SymbolKind::FixedSizeUnpackedArrayType:
        case SymbolKind::DynamicArrayType:
        case SymbolKind::AssociativeArrayType:
        case SymbolKind::QueueType:
        case SymbolKind::UnpackedStructType:
        case SymbolKind::UnpackedUnionType:
            return true;
        default:
            return false;
    }
}

bool Type::isSimpleBitVector() const {
    const Type& ct = getCanonicalType();
    if (ct.isPredefinedInteger() || ct.isScalar())
        return true;

    return ct.kind == SymbolKind::PackedArrayType &&
           ct.as<PackedArrayType>().elementType.isScalar();
}

bool Type::hasFixedRange() const {
    const Type& ct = getCanonicalType();
    return ct.isIntegral() || ct.kind == SymbolKind::FixedSizeUnpackedArrayType;
}

bool Type::isBooleanConvertible() const {
    switch (getCanonicalType().kind) {
        case SymbolKind::NullType:
        case SymbolKind::CHandleType:
        case SymbolKind::StringType:
        case SymbolKind::EventType:
        case SymbolKind::ClassType:
        case SymbolKind::VirtualInterfaceType:
            return true;
        default:
            return isNumeric();
    }
}

bool Type::isArray() const {
    const Type& ct = getCanonicalType();
    switch (ct.kind) {
        case SymbolKind::PackedArrayType:
        case SymbolKind::FixedSizeUnpackedArrayType:
        case SymbolKind::DynamicArrayType:
        case SymbolKind::AssociativeArrayType:
        case SymbolKind::QueueType:
            return true;
        default:
            return false;
    }
}

bool Type::isStruct() const {
    const Type& ct = getCanonicalType();
    switch (ct.kind) {
        case SymbolKind::PackedStructType:
        case SymbolKind::UnpackedStructType:
            return true;
        default:
            return false;
    }
}

bool Type::isBitstreamType(bool destination) const {
    if (isIntegral() || isString())
        return true;

    if (isUnpackedArray()) {
        if (destination && getCanonicalType().kind == SymbolKind::AssociativeArrayType)
            return false;
        return getArrayElementType()->isBitstreamType(destination);
    }

    if (isUnpackedStruct()) {
        auto& us = getCanonicalType().as<UnpackedStructType>();
        for (auto& field : us.membersOfType<FieldSymbol>()) {
            if (!field.getType().isBitstreamType(destination))
                return false;
        }
        return true;
    }

    if (isClass()) {
        if (destination)
            return false;

        auto& ct = getCanonicalType().as<ClassType>();
        if (ct.isInterface)
            return false;

        for (auto& prop : ct.membersOfType<ClassPropertySymbol>()) {
            if (!prop.getType().isBitstreamType(destination))
                return false;
        }
        return true;
    }

    return false;
}

bool Type::isFixedSize() const {
    if (isIntegral() || isFloating())
        return true;

    if (isUnpackedArray()) {
        const auto& ct = getCanonicalType();
        if (ct.kind != SymbolKind::FixedSizeUnpackedArrayType)
            return false;

        return ct.as<FixedSizeUnpackedArrayType>().elementType.isFixedSize();
    }

    if (isUnpackedStruct()) {
        auto& us = getCanonicalType().as<UnpackedStructType>();
        for (auto& field : us.membersOfType<FieldSymbol>()) {
            if (!field.getType().isFixedSize())
                return false;
        }
        return true;
    }

    if (isUnpackedUnion()) {
        auto& us = getCanonicalType().as<UnpackedUnionType>();
        for (auto& field : us.membersOfType<FieldSymbol>()) {
            if (!field.getType().isFixedSize())
                return false;
        }
        return true;
    }

    if (isClass()) {
        auto& ct = getCanonicalType().as<ClassType>();
        if (ct.isInterface)
            return false;

        for (auto& prop : ct.membersOfType<ClassPropertySymbol>()) {
            if (!prop.getType().isFixedSize())
                return false;
        }
        return true;
    }

    return false;
}

bool Type::isSimpleType() const {
    switch (kind) {
        case SymbolKind::PredefinedIntegerType:
        case SymbolKind::ScalarType:
        case SymbolKind::FloatingType:
        case SymbolKind::TypeAlias:
        case SymbolKind::ClassType:
            return true;
        default:
            return false;
    }
}

bool Type::isByteArray() const {
    const Type& ct = getCanonicalType();
    if (!ct.isUnpackedArray())
        return false;

    if (ct.kind == SymbolKind::AssociativeArrayType)
        return false;

    auto& elem = ct.getArrayElementType()->getCanonicalType();
    return elem.isPredefinedInteger() &&
           elem.as<PredefinedIntegerType>().integerKind == PredefinedIntegerType::Byte;
}

bool Type::isUnpackedArray() const {
    switch (getCanonicalType().kind) {
        case SymbolKind::FixedSizeUnpackedArrayType:
        case SymbolKind::DynamicArrayType:
        case SymbolKind::AssociativeArrayType:
        case SymbolKind::QueueType:
            return true;
        default:
            return false;
    }
}

bool Type::isDynamicallySizedArray() const {
    switch (getCanonicalType().kind) {
        case SymbolKind::DynamicArrayType:
        case SymbolKind::AssociativeArrayType:
        case SymbolKind::QueueType:
            return true;
        default:
            return false;
    }
}

bool Type::isTaggedUnion() const {
    auto& ct = getCanonicalType();
    switch (ct.kind) {
        case SymbolKind::PackedUnionType:
            return ct.as<PackedUnionType>().isTagged;
        case SymbolKind::UnpackedUnionType:
            return ct.as<UnpackedUnionType>().isTagged;
        default:
            return false;
    }
}

bool Type::isMatching(const Type& rhs) const {
    // See [6.22.1] for Matching Types.
    const Type* l = &getCanonicalType();
    const Type* r = &rhs.getCanonicalType();

    // If the two types have the same address, they are literally the same type.
    // This handles all built-in types, which are allocated once and then shared,
    // and also handles simple bit vector types that share the same range, signedness,
    // and four-stateness because we uniquify them in the compilation cache.
    // This handles checks [6.22.1] (a), (b), (c), (d), (g), and (h).
    if (l == r || (l->getSyntax() && l->getSyntax() == r->getSyntax()))
        return true;

    // Special casing for type synonyms: real/realtime
    if (l->isFloating() && r->isFloating()) {
        auto lf = l->as<FloatingType>().floatKind;
        auto rf = r->as<FloatingType>().floatKind;
        return (lf == FloatingType::Real || lf == FloatingType::RealTime) &&
               (rf == FloatingType::Real || rf == FloatingType::RealTime);
    }

    // Handle check (e) and (f): matching predefined integers and matching vector types
    // This also handles built-in scalar synonyms and multiple instances of predefined types.
    if (l->isSimpleBitVector() && r->isSimpleBitVector() &&
        (!l->isPackedArray() || !r->isPackedArray())) {
        auto& li = l->as<IntegralType>();
        auto& ri = r->as<IntegralType>();
        return li.isSigned == ri.isSigned && li.isFourState == ri.isFourState &&
               li.getBitVectorRange() == ri.getBitVectorRange();
    }

    // Handle check (f): matching array types
    if (l->isArray() && r->isArray()) {
        // Both arrays must be of the same type (fixed, packed, associative, etc) and
        // their element types must match.
        if (l->kind != r->kind || !l->getArrayElementType()->isMatching(*r->getArrayElementType()))
            return false;

        if (l->kind == SymbolKind::PackedArrayType) {
            // If packed size, ranges must match.
            if (l->as<PackedArrayType>().range != r->as<PackedArrayType>().range)
                return false;
        }
        else if (l->kind == SymbolKind::FixedSizeUnpackedArrayType) {
            // If fixed size, ranges must match.
            if (l->as<FixedSizeUnpackedArrayType>().range !=
                r->as<FixedSizeUnpackedArrayType>().range) {
                return false;
            }
        }
        else if (l->kind == SymbolKind::AssociativeArrayType) {
            // If associative, index types must match.
            auto li = l->getAssociativeIndexType();
            auto ri = r->getAssociativeIndexType();
            if (li) {
                if (!ri || !li->isMatching(*ri))
                    return false;
            }
            else if (ri) {
                return false;
            }
        }

        // Otherwise, the arrays match.
        return true;
    }

    // This is not specified in the standard but people naturally expect it to work:
    // if an enum is declared in an include file and included in multiple compilation
    // units, they will have separate instantiations but should probably still be
    // considered as matching each other.
    if (l->kind == SymbolKind::EnumType && r->kind == SymbolKind::EnumType)
        return isSameEnum(l->as<EnumType>(), r->as<EnumType>());

    if (l->isVirtualInterface() && r->isVirtualInterface()) {
        auto& lv = l->as<VirtualInterfaceType>();
        auto& rv = r->as<VirtualInterfaceType>();
        return &lv.iface == &rv.iface && lv.modport == rv.modport;
    }

    return false;
}

bool Type::isEquivalent(const Type& rhs) const {
    // See [6.22.2] for Equivalent Types
    const Type* l = &getCanonicalType();
    const Type* r = &rhs.getCanonicalType();
    if (l->isMatching(*r))
        return true;

    // (c) packed integral types are equivalent if signedness, four-statedness,
    // and bitwidth are the same.
    if (l->isIntegral() && r->isIntegral() && !l->isEnum() && !r->isEnum()) {
        const auto& li = l->as<IntegralType>();
        const auto& ri = r->as<IntegralType>();
        return li.isSigned == ri.isSigned && li.isFourState == ri.isFourState &&
               li.bitWidth == ri.bitWidth;
    }

    // (d) fixed size unpacked arrays are equivalent if element types are equivalent
    // and ranges are the same width; actual bounds may differ.
    if (l->kind == SymbolKind::FixedSizeUnpackedArrayType &&
        r->kind == SymbolKind::FixedSizeUnpackedArrayType) {
        auto& la = l->as<FixedSizeUnpackedArrayType>();
        auto& ra = r->as<FixedSizeUnpackedArrayType>();
        return la.range.width() == ra.range.width() && la.elementType.isEquivalent(ra.elementType);
    }

    // (e) dynamic arrays, associative arrays, and queues are equivalent if they
    // are the same kind and have equivalent element types.
    if (l->isUnpackedArray() && l->kind == r->kind) {
        // Associative arrays additionally must have the same index type.
        if (l->kind == SymbolKind::AssociativeArrayType) {
            auto li = l->getAssociativeIndexType();
            auto ri = r->getAssociativeIndexType();
            if (li) {
                if (!ri || !li->isEquivalent(*ri))
                    return false;
            }
            else if (ri) {
                return false;
            }
        }

        return l->getArrayElementType()->isEquivalent(*r->getArrayElementType());
    }

    return false;
}

bool Type::isAssignmentCompatible(const Type& rhs) const {
    // See [6.22.3] for Assignment Compatible
    const Type* l = &getCanonicalType();
    const Type* r = &rhs.getCanonicalType();
    if (l->isEquivalent(*r))
        return true;

    // Any integral or floating value can be implicitly converted to a packed integer
    // value or to a floating value.
    if ((l->isIntegral() && !l->isEnum()) || l->isFloating())
        return r->isIntegral() || r->isFloating() || r->isUnbounded();

    if (l->isUnpackedArray() && r->isUnpackedArray()) {
        // Associative arrays are only compatible with each other.
        // This will have already been ruled out by the isEquivalent check above,
        // so we if see them here then they're not compatible.
        if (l->kind == SymbolKind::AssociativeArrayType ||
            r->kind == SymbolKind::AssociativeArrayType) {
            return false;
        }

        // Fixed size unpacked arrays, dynamic arrays, and queues can be assignment
        // compatible with each other, provided element types are equivalent and,
        // if the target is fixed size, the ranges are the same width. We don't
        // need to check the fixed size condition here, since the only way it would
        // matter is if the source (rhs) is dynamically sized, which can't be checked
        // until runtime.
        if (l->kind == r->kind && l->kind == SymbolKind::FixedSizeUnpackedArrayType)
            return false; // !isEquivalent implies unequal widths or non-eqivalent elements
        return l->getArrayElementType()->isEquivalent(*r->getArrayElementType());
    }

    if (l->isClass()) {
        // Null is assignment compatible to all class types.
        if (r->isNull())
            return true;

        // Derived classes can be assigned to parent classes.
        if (r->isDerivedFrom(*l))
            return true;

        // Classes can also be assigned to interface classes that they implement.
        if (r->implements(*l))
            return true;
    }

    if (l->isVirtualInterface()) {
        if (r->isNull())
            return true;

        if (!r->isVirtualInterface())
            return false;

        auto& lv = l->as<VirtualInterfaceType>();
        auto& rv = r->as<VirtualInterfaceType>();
        if (&lv.iface != &rv.iface && lv.iface.getCacheKey() != rv.iface.getCacheKey())
            return false;

        // A virtual interface with no modport selected may be assigned to a
        // virtual interface with a modport selected.
        return (lv.modport == rv.modport) || (lv.modport && !rv.modport);
    }

    // Null can be assigned to chandles and events.
    if (l->isCHandle() || l->isEvent())
        return r->isNull();

    return false;
}

bool Type::isCastCompatible(const Type& rhs) const {
    // See [6.22.4] for Cast Compatible
    const Type* l = &getCanonicalType();
    const Type* r = &rhs.getCanonicalType();
    if (l->isAssignmentCompatible(*r))
        return true;

    if (l->isEnum())
        return r->isIntegral() || r->isFloating();

    if (l->isString())
        return r->isIntegral();

    if (r->isString())
        return l->isIntegral();

    return false;
}

bool Type::isBitstreamCastable(const Type& rhs) const {
    const Type* l = &getCanonicalType();
    const Type* r = &rhs.getCanonicalType();
    if (l->isBitstreamType(true) && r->isBitstreamType()) {
        if (l->isFixedSize() && r->isFixedSize())
            return l->bitstreamWidth() == r->bitstreamWidth();
        else
            return Bitstream::dynamicSizesMatch(*l, *r);
    }
    return false;
}

bool Type::isDerivedFrom(const Type& base) const {
    const Type* d = &getCanonicalType();
    const Type* b = &base.getCanonicalType();
    if (!b->isClass())
        return false;

    while (d && d->isClass()) {
        d = d->as<ClassType>().getBaseClass();
        if (d == b)
            return true;
    }

    // Allow error types to be convertible / derivable from anything else,
    // to prevent knock-on errors from being reported.
    if (d && d->isError())
        return true;

    return false;
}

bool Type::implements(const Type& ifaceClass) const {
    const Type* c = &getCanonicalType();
    if (!c->isClass())
        return false;

    for (auto iface : c->as<ClassType>().getImplementedInterfaces()) {
        if (iface->isMatching(ifaceClass))
            return true;
    }

    return false;
}

bitmask<IntegralFlags> Type::getIntegralFlags() const {
    bitmask<IntegralFlags> flags;
    if (!isIntegral())
        return flags;

    const IntegralType& it = getCanonicalType().as<IntegralType>();
    if (it.isSigned)
        flags |= IntegralFlags::Signed;
    if (it.isFourState)
        flags |= IntegralFlags::FourState;
    if (it.isDeclaredReg())
        flags |= IntegralFlags::Reg;

    return flags;
}

ConstantValue Type::getDefaultValue() const {
    GetDefaultVisitor visitor;
    return visit(visitor);
}

ConstantRange Type::getFixedRange() const {
    const Type& t = getCanonicalType();
    if (t.isIntegral())
        return t.as<IntegralType>().getBitVectorRange();

    if (t.kind == SymbolKind::FixedSizeUnpackedArrayType)
        return t.as<FixedSizeUnpackedArrayType>().range;

    return {};
}

const Type* Type::getArrayElementType() const {
    const Type& t = getCanonicalType();
    switch (t.kind) {
        case SymbolKind::PackedArrayType:
            return &t.as<PackedArrayType>().elementType;
        case SymbolKind::FixedSizeUnpackedArrayType:
            return &t.as<FixedSizeUnpackedArrayType>().elementType;
        case SymbolKind::DynamicArrayType:
            return &t.as<DynamicArrayType>().elementType;
        case SymbolKind::AssociativeArrayType:
            return &t.as<AssociativeArrayType>().elementType;
        case SymbolKind::QueueType:
            return &t.as<QueueType>().elementType;
        default:
            return nullptr;
    }
}

const Type* Type::getAssociativeIndexType() const {
    const Type& t = getCanonicalType();
    if (t.kind == SymbolKind::AssociativeArrayType)
        return t.as<AssociativeArrayType>().indexType;
    return nullptr;
}

bool Type::canBeStringLike() const {
    const Type& t = getCanonicalType();
    return t.isIntegral() || t.isString() || t.isByteArray();
}

bool Type::isIterable() const {
    const Type& t = getCanonicalType();
    return (t.hasFixedRange() || t.isArray() || t.isString()) && !t.isScalar();
}

bool Type::isValidForRand(RandMode mode) const {
    if ((isIntegral() || isNull()) && !isTaggedUnion())
        return true;

    if (isArray())
        return getArrayElementType()->isValidForRand(mode);

    if (isClass() || isUnpackedStruct())
        return mode == RandMode::Rand;

    return false;
}

bool Type::isValidForDPIReturn() const {
    switch (getCanonicalType().kind) {
        case SymbolKind::VoidType:
        case SymbolKind::FloatingType:
        case SymbolKind::CHandleType:
        case SymbolKind::StringType:
        case SymbolKind::ScalarType:
        case SymbolKind::PredefinedIntegerType:
            return true;
        default:
            return false;
    }
}

bool Type::isValidForDPIArg() const {
    auto& ct = getCanonicalType();
    if (ct.isIntegral() || ct.isFloating() || ct.isString() || ct.isCHandle() || ct.isVoid())
        return true;

    if (ct.kind == SymbolKind::FixedSizeUnpackedArrayType)
        return ct.as<FixedSizeUnpackedArrayType>().elementType.isValidForDPIArg();

    if (ct.isUnpackedStruct()) {
        for (auto& field : ct.as<UnpackedStructType>().membersOfType<FieldSymbol>()) {
            if (!field.getType().isValidForDPIArg())
                return false;
        }
        return true;
    }

    return false;
}

bool Type::isValidForSequence() const {
    // Type must be cast compatible with an integral type to be valid.
    auto& ct = getCanonicalType();
    return ct.isIntegral() || ct.isString() || ct.isFloating();
}

ConstantValue Type::coerceValue(const ConstantValue& value) const {
    if (isIntegral())
        return value.convertToInt(getBitWidth(), isSigned(), isFourState());

    if (isFloating()) {
        if (getBitWidth() == 32)
            return value.convertToShortReal();
        else
            return value.convertToReal();
    }

    if (isString())
        return value.convertToStr();

    return nullptr;
}

std::string Type::toString() const {
    TypePrinter printer;
    printer.append(*this);
    return printer.toString();
}

size_t Type::hash() const {
    size_t h = size_t(kind);
    auto& ct = getCanonicalType();
    if (ct.isScalar()) {
        auto sk = ct.as<ScalarType>().scalarKind;
        if (sk == ScalarType::Reg)
            sk = ScalarType::Logic;
        hash_combine(h, sk);
    }
    else if (ct.isFloating()) {
        auto fk = ct.as<FloatingType>().floatKind;
        if (fk == FloatingType::RealTime)
            fk = FloatingType::Real;
        hash_combine(h, fk);
    }
    else if (ct.isIntegral()) {
        auto& it = ct.as<IntegralType>();
        hash_combine(h, it.isSigned, it.isFourState, it.bitWidth);
    }
    else if (ct.kind == SymbolKind::FixedSizeUnpackedArrayType) {
        auto& uat = ct.as<FixedSizeUnpackedArrayType>();
        hash_combine(h, uat.range.left, uat.range.right, uat.elementType.hash());
    }
    else if (ct.kind == SymbolKind::DynamicArrayType) {
        auto& dat = ct.as<DynamicArrayType>();
        hash_combine(h, dat.elementType.hash());
    }
    else if (ct.kind == SymbolKind::AssociativeArrayType) {
        auto& aat = ct.as<AssociativeArrayType>();
        hash_combine(h, aat.elementType.hash());
        if (aat.indexType)
            hash_combine(h, aat.indexType->hash());
    }
    else if (ct.kind == SymbolKind::QueueType) {
        auto& qt = ct.as<QueueType>();
        hash_combine(h, qt.elementType.hash(), qt.maxBound);
    }
    else if (ct.kind == SymbolKind::VirtualInterfaceType) {
        auto& vi = ct.as<VirtualInterfaceType>();
        hash_combine(h, &vi.iface);
        hash_combine(h, vi.modport);
    }
    else {
        h = std::hash<const Type*>()(&ct);
    }
    return h;
}

const Type* Type::getCommonBase(const Type& left, const Type& right) {
    const Type* l = &left.getCanonicalType();
    const Type* r = &right.getCanonicalType();
    if (!l->isClass() || !r->isClass())
        return nullptr;

    SmallSet<const Type*, 8> parents;
    while (true) {
        if (l == r)
            return r;

        parents.emplace(l);
        l = l->as<ClassType>().getBaseClass();
        if (!l)
            break;

        if (l->isError())
            return l;

        l = &l->getCanonicalType();
    }

    while (true) {
        if (auto it = parents.find(r); it != parents.end())
            return r;

        r = r->as<ClassType>().getBaseClass();
        if (!r)
            return nullptr;

        if (r->isError())
            return r;

        r = &r->getCanonicalType();
    }
}

const Type& Type::fromSyntax(Compilation& compilation, const DataTypeSyntax& node,
                             const BindContext& context, const Type* typedefTarget) {
    switch (node.kind) {
        case SyntaxKind::BitType:
        case SyntaxKind::LogicType:
        case SyntaxKind::RegType:
            return IntegralType::fromSyntax(compilation, node.as<IntegerTypeSyntax>(), context);
        case SyntaxKind::ByteType:
        case SyntaxKind::ShortIntType:
        case SyntaxKind::IntType:
        case SyntaxKind::LongIntType:
        case SyntaxKind::IntegerType:
        case SyntaxKind::TimeType: {
            auto& its = node.as<IntegerTypeSyntax>();
            if (!its.dimensions.empty()) {
                // Error but don't fail out; just remove the dims and keep trucking
                auto& diag = context.addDiag(diag::PackedDimsOnPredefinedType,
                                             its.dimensions[0]->openBracket.location());
                diag << LexerFacts::getTokenKindText(its.keyword.kind);
            }

            if (!its.signing)
                return compilation.getType(node.kind);

            return getPredefinedType(compilation, node.kind,
                                     its.signing.kind == TokenKind::SignedKeyword);
        }
        case SyntaxKind::RealType:
        case SyntaxKind::RealTimeType:
        case SyntaxKind::ShortRealType:
        case SyntaxKind::StringType:
        case SyntaxKind::CHandleType:
        case SyntaxKind::EventType:
        case SyntaxKind::VoidType:
        case SyntaxKind::Untyped:
        case SyntaxKind::PropertyType:
        case SyntaxKind::SequenceType:
            return compilation.getType(node.kind);
        case SyntaxKind::EnumType:
            return EnumType::fromSyntax(compilation, node.as<EnumTypeSyntax>(), context,
                                        typedefTarget);
        case SyntaxKind::StructType: {
            const auto& structUnion = node.as<StructUnionTypeSyntax>();
            return structUnion.packed
                       ? PackedStructType::fromSyntax(compilation, structUnion, context)
                       : UnpackedStructType::fromSyntax(context, structUnion);
        }
        case SyntaxKind::UnionType: {
            const auto& structUnion = node.as<StructUnionTypeSyntax>();
            return structUnion.packed
                       ? PackedUnionType::fromSyntax(compilation, structUnion, context)
                       : UnpackedUnionType::fromSyntax(context, structUnion);
        }
        case SyntaxKind::NamedType:
            return lookupNamedType(compilation, *node.as<NamedTypeSyntax>().name, context,
                                   typedefTarget != nullptr);
        case SyntaxKind::ImplicitType: {
            auto& implicit = node.as<ImplicitTypeSyntax>();
            return IntegralType::fromSyntax(compilation, SyntaxKind::LogicType, implicit.dimensions,
                                            implicit.signing.kind == TokenKind::SignedKeyword,
                                            context);
        }
        case SyntaxKind::TypeReference: {
            auto& expr = Expression::bind(*node.as<TypeReferenceSyntax>().expr,
                                          context.resetFlags(BindFlags::NoHierarchicalNames),
                                          BindFlags::AllowDataType);
            return *expr.type;
        }
        case SyntaxKind::VirtualInterfaceType:
            return VirtualInterfaceType::fromSyntax(context, node.as<VirtualInterfaceTypeSyntax>());
        default:
            THROW_UNREACHABLE;
    }
}

const Type& Type::fromSyntax(Compilation& compilation, const Type& elementType,
                             const SyntaxList<VariableDimensionSyntax>& dimensions,
                             const BindContext& context) {
    if (elementType.isError())
        return elementType;

    switch (elementType.getCanonicalType().kind) {
        case SymbolKind::SequenceType:
        case SymbolKind::PropertyType:
        case SymbolKind::UntypedType:
            context.addDiag(diag::InvalidArrayElemType, dimensions.sourceRange()) << elementType;
            return compilation.getErrorType();
        default:
            break;
    }

    const Type* result = &elementType;
    size_t count = dimensions.size();
    for (size_t i = 0; i < count; i++) {
        auto& syntax = *dimensions[count - i - 1];
        auto dim = context.evalDimension(syntax, /* requireRange */ false, /* isPacked */ false);

        Type* next = nullptr;
        switch (dim.kind) {
            case DimensionKind::Unknown:
                return compilation.getErrorType();
            case DimensionKind::Range:
            case DimensionKind::AbbreviatedRange:
                next = compilation.emplace<FixedSizeUnpackedArrayType>(*result, dim.range);
                break;
            case DimensionKind::Dynamic:
                next = compilation.emplace<DynamicArrayType>(*result);
                break;
            case DimensionKind::Associative:
                next = compilation.emplace<AssociativeArrayType>(*result, dim.associativeType);
                break;
            case DimensionKind::Queue:
                next = compilation.emplace<QueueType>(*result, dim.queueMaxSize);
                break;
        }

        next->setSyntax(syntax);
        result = next;
    }

    return *result;
}

bool Type::isKind(SymbolKind kind) {
    switch (kind) {
        case SymbolKind::PredefinedIntegerType:
        case SymbolKind::ScalarType:
        case SymbolKind::FloatingType:
        case SymbolKind::EnumType:
        case SymbolKind::PackedArrayType:
        case SymbolKind::FixedSizeUnpackedArrayType:
        case SymbolKind::DynamicArrayType:
        case SymbolKind::AssociativeArrayType:
        case SymbolKind::QueueType:
        case SymbolKind::PackedStructType:
        case SymbolKind::UnpackedStructType:
        case SymbolKind::PackedUnionType:
        case SymbolKind::UnpackedUnionType:
        case SymbolKind::ClassType:
        case SymbolKind::VoidType:
        case SymbolKind::NullType:
        case SymbolKind::CHandleType:
        case SymbolKind::StringType:
        case SymbolKind::EventType:
        case SymbolKind::UnboundedType:
        case SymbolKind::TypeRefType:
        case SymbolKind::UntypedType:
        case SymbolKind::SequenceType:
        case SymbolKind::PropertyType:
        case SymbolKind::VirtualInterfaceType:
        case SymbolKind::TypeAlias:
        case SymbolKind::ErrorType:
            return true;
        default:
            return false;
    }
}

void Type::resolveCanonical() const {
    ASSERT(kind == SymbolKind::TypeAlias);
    canonical = this;
    do {
        canonical = &canonical->as<TypeAliasType>().targetType.getType();
    } while (canonical->isAlias());
}

const Type& Type::lookupNamedType(Compilation& compilation, const NameSyntax& syntax,
                                  const BindContext& context, bool isTypedefTarget) {
    bitmask<LookupFlags> flags = LookupFlags::Type;
    if (isTypedefTarget)
        flags |= LookupFlags::TypedefTarget;

    LookupResult result;
    Lookup::name(syntax, context, flags, result);

    if (result.hasError())
        compilation.addDiagnostics(result.getDiagnostics());

    return fromLookupResult(compilation, result, syntax.sourceRange(), context);
}

const Type& Type::fromLookupResult(Compilation& compilation, const LookupResult& result,
                                   SourceRange sourceRange, const BindContext& context) {
    const Symbol* symbol = result.found;
    if (!symbol)
        return compilation.getErrorType();

    if (!symbol->isType()) {
        context.addDiag(diag::NotAType, sourceRange) << symbol->name;
        return compilation.getErrorType();
    }

    const Type* finalType = &symbol->as<Type>();
    size_t count = result.selectors.size();
    for (size_t i = 0; i < count; i++) {
        // TODO: handle dotted selectors
        auto selectSyntax = std::get<const ElementSelectSyntax*>(result.selectors[count - i - 1]);
        auto dim = context.evalPackedDimension(*selectSyntax);
        if (!dim)
            return compilation.getErrorType();

        finalType = &PackedArrayType::fromSyntax(*context.scope, *finalType, *dim, *selectSyntax);
    }

    return *finalType;
}

const Type& Type::getPredefinedType(Compilation& compilation, SyntaxKind kind, bool isSigned) {
    auto& predef = compilation.getType(kind).as<IntegralType>();
    if (isSigned == predef.isSigned)
        return predef;

    if (predef.kind == SymbolKind::ScalarType)
        return *compilation.emplace<ScalarType>(predef.as<ScalarType>().scalarKind, isSigned);

    return *compilation.emplace<PredefinedIntegerType>(
        predef.as<PredefinedIntegerType>().integerKind, isSigned);
}

Diagnostic& operator<<(Diagnostic& diag, const Type& arg) {
    ASSERT(!arg.isError());
    diag.args.emplace_back(&arg);
    return diag;
}

} // namespace slang
