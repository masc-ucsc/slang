//------------------------------------------------------------------------------
//! @file NumberParser.h
//! @brief Helper type to parse numeric literals
//
// File is under the MIT license; see LICENSE for details
//------------------------------------------------------------------------------
#pragma once

#include "slang/diagnostics/Diagnostics.h"
#include "slang/diagnostics/NumericDiags.h"
#include "slang/numeric/SVInt.h"
#include "slang/parsing/Token.h"
#include "slang/syntax/SyntaxFacts.h"
#include "slang/text/SourceLocation.h"
#include "slang/util/SmallVector.h"

namespace slang {

class NumberParser {
public:
    NumberParser(Diagnostics& diagnostics, BumpAllocator& alloc);

    struct IntResult {
        Token size;
        Token base;
        Token value;
        bool isSimple = true;

        static IntResult simple(Token value) { return { Token(), Token(), value, true }; }

        static IntResult vector(Token size, Token base, Token value) {
            return { size, base, value, false };
        }
    };

    template<typename TStream>
    IntResult parseSimpleInt(TStream& stream) {
        auto token = stream.consume();
        if (token.intValue() > INT32_MAX)
            addDiag(diag::SignedIntegerOverflow, token.location());
        return IntResult::simple(token);
    }

    template<typename TStream>
    IntResult parseInteger(TStream& stream) {
        Token sizeToken;
        Token baseToken;

        auto token = stream.consume();
        if (token.kind == TokenKind::IntegerBase) {
            baseToken = token;
            startVector(baseToken, Token());
        }
        else {
            if (!stream.peek(TokenKind::IntegerBase)) {
                if (token.intValue() > INT32_MAX)
                    addDiag(diag::SignedIntegerOverflow, token.location());
                return IntResult::simple(token);
            }

            sizeToken = token;
            baseToken = stream.consume();
            startVector(baseToken, sizeToken);
        }

        // At this point we expect to see vector digits, but they could be split out into other
        // token types because of hex literals.
        auto first = stream.peek();
        if (!SyntaxFacts::isPossibleVectorDigit(first.kind))
            return reportMissingDigits(sizeToken, baseToken, first);

        int count = 0;
        Token next = first;
        firstLocation = first.location();

        do {
            count++;
            int index = append(next, count == 1);
            stream.consume();

            if (index >= 0) {
                // This handles a really obnoxious case: 'h 3e+2
                // The second token is initially lexed as a real literal, but we need to split
                // it apart here now that we know it's a hex literal and put the remaining (new)
                // tokens back on the parser's stack.
                stream.handleExponentSplit(next, (size_t)index);

                // Bump the count so that we definitely take the modified raw text
                // instead of trying to use the initial token's raw directly.
                count++;
                break;
            }

            next = stream.peek();
        } while (SyntaxFacts::isPossibleVectorDigit(next.kind) && next.trivia().empty());

        return IntResult::vector(sizeToken, baseToken, finishValue(first, count == 1));
    }

    template<typename TStream>
    Token parseReal(TStream& stream) {
        // have to check for overflow here, now that we know this is actually a real
        auto literal = stream.consume();
        if (literal.numericFlags().outOfRange()) {
            if (literal.realValue() == 0) {
                addDiag(diag::RealLiteralUnderflow, literal.location())
                    << real_t(std::numeric_limits<double>::denorm_min());
            }
            else {
                ASSERT(!std::isfinite(literal.realValue()));
                addDiag(diag::RealLiteralOverflow, literal.location())
                    << real_t(std::numeric_limits<double>::max());
            }
        }
        return literal;
    }

private:
    void startVector(Token baseToken, Token sizeToken);
    int append(Token token, bool isFirst);
    Token finishValue(Token firstToken, bool singleToken);
    void addDigit(logic_t digit, int maxValue);
    Diagnostic& addDiag(DiagCode code, SourceLocation location);
    IntResult reportMissingDigits(Token sizeToken, Token baseToken, Token first);

    bitwidth_t sizeBits = 0;
    LiteralBase literalBase = LiteralBase::Binary;
    SourceLocation firstLocation;
    bool signFlag = false;
    bool hasUnknown = false;
    bool valid = false;
    SVInt decimalValue;
    Diagnostics& diagnostics;
    BumpAllocator& alloc;
    SmallVectorSized<logic_t, 16> digits;
    SmallVectorSized<char, 64> text;
};

} // namespace slang
