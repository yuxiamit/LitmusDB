#include <string>
#include "litmus-template.h"

using namespace std;

const std::string LM_prog =
//"{DEF_ADDR_READ}\n"
//"{DEF_ADDR_WRITE}\n"
"typedef unsigned int uint32_t;\n"
//"#include <stdint.h>\n"
//"#include <gmp.h>\n"
//"#define H(addr, val) (addr*val) // dummy hash function for now\n"
//"#define H_addr(addr) (addr) \n"
"\n"
"// this file serves as a template for a single transaction that reads two rows.\n"
"\n"
"struct In {{INPUT_STRUCTURE}};\n"
"struct Out {uint32_t ok;};\n"
"\n"
"uint32_t ipow(uint32_t base_t, uint32_t exp)\n"
"{\n"
"    uint32_t result = 1;\n"
"    uint32_t base = base_t - 1;"
"    uint32_t i;\n"
"    // we cannot use while(1) here\n"
"    for(i=0;i<32;i++)\n"
"    {\n" 
// TODO: unroll the following into 16 bits, now 8 bits
//"        if (exp % 2 == 1)\n"
"            result = result * (1 + exp % 2 * base) % {N};\n"
"        exp = exp / 2;\n"
"        base = base * base % {N};\n"
"    }\n"
"    return result;\n"
"}\n"
"\n"
"void compute(struct In* input, struct Out* output){ // a simple YCSB construction.\n"
"    uint32_t localDigest = {G};\n" // = g^1.
"    uint32_t allpass = 1;\n"
"    uint32_t txnpass = 0;\n"
"    {TXN_CONTENT}\n"
"   output->ok = allpass;\n"
"}\n";

const std::string LM_addrRead =
"#define READ_ADDR_{INDEX}_{RI} {READ_ADDR}\n";

const std::string LM_addrWrite =
"#define WRITE_ADDR_{INDEX}_{WI} {WRITE_ADDR}\n";

const std::string LM_inputStructure =
"uint32_t val_{INDEX}_{RI}; uint32_t acc1_{INDEX}_{RI}; uint32_t acc2_{INDEX}_{RI}; uint32_t prod_{INDEX}_{RI}; uint32_t acc_prime_{INDEX}_{RI}; uint32_t A_{INDEX}_{RI}; uint32_t B_{INDEX}_{RI};";

const std::string LM_transactionBody = 
"    // starting transaction {INDEX};\n"
"    "
"       {READ_CHECK}\n"
"    "
"    {\n"
"        // run the content of the transaction\n"
"        {TXN_LOGIC}\n"
"        // update the digest\n"
"        {UPDATE_WRITE}\n"
"        // calculate the allpass flag\n"
//"        txnpass = 1;\n"
"    }\n"
"    {ELSE_BRANCH}\n"
"    // ending transaction {INDEX};\n";

const std::string LM_txnLogic = 
"        uint32_t outval_{WI} = input->val_{INDEX}_{WI} * 2;\n";

// here we assume that every transaction does write operations no more than reads

const std::string LM_hAddr = 
"({READ_ADDR})";

const std::string LM_h1 = 
"({READ_ADDR} * input->val_{INDEX}_{RI})";

const std::string LM_h2 = 
"({WRITE_ADDR} * outval_{WI})";

// H1 = H({READ_ADDR}, input->val_{INDEX}_{RI})
// H2 = H({WRITE_ADDR}, outval_{WI})

const std::string LM_readValCheckInit = "input->val_{INDEX}_{RI}==0\n"; // non-membership are batched elsewhere.

//const std::string LM_readValCheckInit = "input->val_{INDEX}_{RI}\n"; // non-membership are batched elsewhere.


const std::string LM_readValCheckNonMemBatched = "ipow(localDigest, input->A_{INDEX}_{RI}) * ipow({G}, {H_ADDR} * input->B_{INDEX}_{RI}) % {N} == {G}";


const std::string LM_readValCheck =
//"        (input->val_{INDEX}_{RI}==0 && ipow(localDigest, input->A_{INDEX}_{RI}) * ipow({G}, {H_ADDR} * input->B_{INDEX}_{RI}) % {N} == {G} ) ||(\n"
"        ipow(input->acc1_{INDEX}_{RI}, {H_1})==input->acc2_{INDEX}_{RI} &&\n"
"        ipow(input->acc2_{INDEX}_{RI}, input->prod_{INDEX}_{RI})==localDigest &&\n"
"        ipow({G}, input->prod_{INDEX}_{RI}) == input->acc_prime_{INDEX}_{RI} &&\n"
"        ipow(input->acc_prime_{INDEX}_{RI}, input->A_{INDEX}_{RI}) * ipow({G}, {H_ADDR} * input->B_{INDEX}_{RI}) % {N} == {G}\n"
//"        )\n"
;

const std::string LM_updateWrite =
"        localDigest = ipow(localDigest, {H_2});\n";

// grabbed from https://stackoverflow.com/questions/3418231/replace-part-of-a-string-with-another-string/3418285
void replaceAll(std::string& str, const std::string& from, const std::string& to) {
    size_t start_pos = 0;
    while((start_pos = str.find(from, start_pos)) != std::string::npos) {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length(); // In case 'to' contains 'from', like replacing 'x' with 'yx'
    }
}

void replaceOnce(std::string& str, const std::string& from, const std::string& to) {
    size_t start_pos = str.find(from, 0);
    if(start_pos != std::string::npos)
        str.replace(start_pos, from.length(), to);
}

string renderWrappedTxn(
    //const string defAddrRead, const string defAddrWrite, 
const string inputStructure, const string txnContent)
{
    string temp = LM_prog;
    //replaceOnce(temp, "{DEF_ADDR_READ}", defAddrRead);
    //replaceOnce(temp, "{DEF_ADDR_WRITE}", defAddrWrite);
    replaceOnce(temp, "{INPUT_STRUCTURE}", inputStructure);
    replaceOnce(temp, "{TXN_CONTENT}", txnContent);
    replaceAll(temp, "{N}", to_string(__LTM_N));
    replaceAll(temp, "{G}", to_string(__LTM_G));
    return temp;
}

string renderTxnBody(const string readCheck, const string txnLogic, const string updateWrite, uint32_t index)
{
    string temp = LM_transactionBody;
    replaceAll(temp, "{INDEX}", to_string(index));
    if(readCheck.size() > 0)
    {
        replaceOnce(temp, "{READ_CHECK}", "if(" + readCheck + ")");
        replaceOnce(temp, "{ELSE_BRANCH}", "{allpass = 0;}");
    }
    else
    {
        // no if-else happening here
        replaceOnce(temp, "{READ_CHECK}", "");
        replaceOnce(temp, "{ELSE_BRANCH}", "");
    }
    
    replaceOnce(temp, "{TXN_LOGIC}", txnLogic);
    replaceOnce(temp, "{UPDATE_WRITE}", updateWrite);
    return temp;
}

string renderTemplateREAD(const string tempRead, uint32_t index, uint32_t ri, uint32_t read_addr)
{
    string temp = tempRead;
    
    replaceOnce(temp, "{H_1}", LM_h1);
    replaceAll(temp, "{H_ADDR}", LM_hAddr);
    replaceAll(temp, "{INDEX}", to_string(index));
    replaceAll(temp, "{RI}", to_string(ri));
    replaceAll(temp, "{READ_ADDR}", to_string(read_addr));
    return temp;
}

string renderTemplateREAD(const string tempRead, uint32_t index, uint32_t ri)
{
    string temp = tempRead;
    replaceAll(temp, "{INDEX}", to_string(index));
    replaceAll(temp, "{RI}", to_string(ri));
    return temp;
}

string renderTemplateWrite(const string tempWrite, uint32_t index, uint32_t wi, uint32_t write_addr)
{
    string temp = tempWrite;
    replaceOnce(temp, "{H_2}", LM_h2);
    replaceAll(temp, "{INDEX}", to_string(index));
    replaceAll(temp, "{WI}", to_string(wi));
    replaceAll(temp, "{WRITE_ADDR}", to_string(write_addr));
    return temp;
}

string renderTemplateWriteH2(const string tempWrite, uint32_t wi)
{
    string temp = tempWrite;
    replaceOnce(temp, "{H_2}", to_string(wi));
    return temp;
}

string renderTemplateWrite(const string tempWrite, uint32_t index, uint32_t wi)
{
    string temp = tempWrite;
    replaceAll(temp, "{INDEX}", to_string(index));
    replaceAll(temp, "{WI}", to_string(wi));
    return temp;
}
