#pragma once
#include <cstdint>
#include <limits>
#include <charconv>
#include <optional>
#include <string_view>
namespace swml_math {
enum class Rounding { down, up };
inline bool add(uint64_t a,uint64_t b,uint64_t& result){if(b>std::numeric_limits<uint64_t>::max()-a)return false;result=a+b;return true;}
inline bool subtract(uint64_t a,uint64_t b,uint64_t& result){if(b>a)return false;result=a-b;return true;}
// Exact 128-bit intermediate, implemented with standard 64-bit operations so
// MSVC and Linux compilers agree. Output stays unchanged on overflow/zero divisor.
inline bool multiply_divide(uint64_t a,uint64_t b,uint64_t divisor,Rounding rounding,uint64_t& result){
    if(!divisor)return false;
    uint64_t a0=uint32_t(a),a1=a>>32,b0=uint32_t(b),b1=b>>32;
    uint64_t p00=a0*b0,p01=a0*b1,p10=a1*b0,p11=a1*b1;
    uint64_t middle=(p00>>32)+uint32_t(p01)+uint32_t(p10);
    uint64_t low=(middle<<32)|uint32_t(p00),high=p11+(p01>>32)+(p10>>32)+(middle>>32);
    if(high>=divisor)return false;
    uint64_t quotient=0,remainder=high;
    for(int bit=63;bit>=0;--bit){
        bool carry=(remainder>>63)!=0;remainder=(remainder<<1)|((low>>bit)&1);
        bool take=carry||remainder>=divisor;if(take)remainder-=divisor;quotient=(quotient<<1)|(take?1u:0u);
    }
    if(rounding==Rounding::up&&remainder){if(quotient==std::numeric_limits<uint64_t>::max())return false;++quotient;}
    result=quotient;return true;
}
// Use decimal strings for exact 64-bit identifiers/quantities in JSON metadata.
// The small JSON DOM's number type is IEEE double; binary message fields are uint64.
inline std::optional<uint64_t> parse_uint64(std::string_view text){
    if(text.empty()||text.size()>20||(text.size()>1&&text.front()=='0'))return {};
    uint64_t value=0;auto converted=std::from_chars(text.data(),text.data()+text.size(),value);
    if(converted.ec!=std::errc{}||converted.ptr!=text.data()+text.size())return {};return value;
}
}
