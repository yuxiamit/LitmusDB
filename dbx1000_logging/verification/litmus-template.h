#include <string>

#define __LTM_N (2577668663)  // replaced with a 128-bit N
//#define __LTM_P (56611) // known to the client only
//#define __LTM_Q (45533) // known to the client only
//#define __LTM_G 7 // easier for debug
#define __LTM_G (5507373622554497461UL)

using namespace std;

extern const std::string LM_prog;
extern const std::string LM_addrRead;
extern const std::string LM_addrWrite;
extern const std::string LM_inputStructure;
extern const std::string LM_transactionBody;
extern const std::string LM_txnLogic;
extern const std::string LM_readValCheckInit;
extern const std::string LM_readValCheckNonMemBatched;
extern const std::string LM_readValCheck;
extern const std::string LM_updateWrite;

string renderTemplateREAD(const string tempRead, uint32_t index, uint32_t ri, uint32_t read_addr);

string renderTemplateREAD(const string tempRead, uint32_t index, uint32_t ri);

string renderTemplateWriteH2(const string tempWrite, uint32_t wi);

string renderTemplateWrite(const string tempWrite, uint32_t index, uint32_t wi, uint32_t write_addr);

string renderTemplateWrite(const string tempWrite, uint32_t index, uint32_t wi);

string renderTxnBody(const string readCheck, const string txnLogic, const string updateWrite, uint32_t index);

string renderWrappedTxn(
	//const string defAddrRead, const string defAddrWrite, 
const string inputStructure, const string txnContent);

void replaceOnce(std::string& str, const std::string& from, const std::string& to);

void replaceAll(std::string& str, const std::string& from, const std::string& to);
