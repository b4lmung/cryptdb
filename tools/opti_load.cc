/*1. store data as column files, and restore data as plaintext insert query
* 2. plaintext insert query should be able to recover directly
* 3. should be able to used exsisting data to reduce the computation overhead(to be implemented)
*/
#include <stdlib.h>
#include <stdio.h>
#include <string>
#include <map>
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <algorithm>
#include "wrapper/reuse.hh"
#include "wrapper/common.hh"
#include "wrapper/insert_lib.hh"
#include "util/constants.hh"
#include "util/timer.hh"
#include "util/log.hh"

using std::cout;
using std::cin;
using std::endl;
using std::vector;
using std::string;
using std::to_string;
char * globalEsp=NULL;
int num_of_pipe = 4;
//global map, for each client, we have one WrapperState which contains ProxyState.
static
std::string logfilePrefix = "final_load";

static
std::string logfileName = logfilePrefix+constGlobalConstants.logFile+std::to_string(time(NULL));

static
logToFile glog(logfileName);


//This connection mimics the behaviour of MySQL-Proxy
Connect  *globalConn;

fullBackUp *gfb;

struct batch{
    vector<string> field_names;
    vector<int> field_types;
    vector<int> field_lengths;
};

batch *ggbt;



static
std::vector<std::string>
getDbTables(std::string db) {
    executeAndGetColumnData(globalConn,std::string("use ")+db);
    MySQLColumnData resraw = executeAndGetColumnData(globalConn,"show tables");
    return resraw.columnData[0];
}

/*should choose the right decryption onion*/

static
std::shared_ptr<ReturnMeta> getReturnMeta(std::vector<FieldMeta*> fms,
                                      std::vector<FieldMetaTrans> &tfds){
    assert(fms.size()==tfds.size());
    std::shared_ptr<ReturnMeta> myReturnMeta = std::make_shared<ReturnMeta>();
    int pos=0;
    //construct OLK
    for(auto i=0u;i<tfds.size();i++){
        //the order is DET,OPE,ASHE,AGG. other onions are not decryptable!!
        int index = getDecryptionOnionIndex(tfds[i]);
        if(index==-1) assert(0);
        onion o = tfds[i].getChoosenOnionO()[index];
        glog<<"choosenDecryptionOnion: "<<TypeText<onion>::toText(o)<<"\n";
        SECLEVEL l = tfds[i].getOriginalFieldMeta()->getOnionMeta(o)->getSecLevel();
        FieldMeta *k = tfds[i].getOriginalFieldMeta();
        OLK curOLK(o,l,k);
        bool use_salt = false;
        if(needsSalt(curOLK))
            use_salt = true;
	addToReturn(myReturnMeta.get(),pos++,curOLK,use_salt,k->getFieldName());
        if(use_salt)
            addSaltToReturn(myReturnMeta.get(),pos++);
        //used to record choosen field lengths, onion names , and field types
        ggbt->field_types.push_back(tfds[i].getChoosenFieldTypes()[index]);
        ggbt->field_names.push_back(tfds[i].getChoosenOnionName()[index]);
        ggbt->field_lengths.push_back(tfds[i].getChoosenFieldLengths()[index]);
        if(use_salt){
            ggbt->field_types.push_back(tfds[i].getSaltType());
            ggbt->field_names.push_back(tfds[i].getSaltName());
            ggbt->field_lengths.push_back(tfds[i].getSaltLength());
        }
    }
    return myReturnMeta;
}
/*init global full backup. */
static
void initGfb(std::vector<FieldMetaTrans> &res,std::string db,std::string table){
    vector<string> field_names;
    vector<int> field_types;
    vector<int> field_lengths;
    /*choosen onions should all be included in gfb-> salt is also included
      it's hard to decide whether a FieldMetaTrans has salt because the senmantic is different from that of FieldMeta.
    */
    for(auto &item:res){
        //only choose onions that are used.
        for(auto i=0u;i<item.getChoosenOnionName().size();i++){
            onion o = item.getChoosenOnionO()[i];
            if( ((o==oDET)&&(constGlobalConstants.useDET==true)) ||
                ((o==oOPE)&&(constGlobalConstants.useOPE==true)) ||
                ((o==oASHE)&&(constGlobalConstants.useASHE==true)) ||
                ((o==oAGG)&&(constGlobalConstants.useHOM==true)) ||
                ((o==oSWP)&&(constGlobalConstants.useSWP==true)) ) {
                field_names.push_back(item.getChoosenOnionName()[i]);
                field_types.push_back(item.getChoosenFieldTypes()[i]);
                field_lengths.push_back(item.getChoosenFieldLengths()[i]);
                glog<<"usedField: "<<item.getChoosenOnionName()[i]<<"\n";
            }
        }
        if(item.getHasSalt()){
            field_names.push_back(item.getSaltName());
            field_types.push_back(item.getSaltType());
            field_lengths.push_back(item.getSaltLength());
            glog<<"useSalt: "<<item.getSaltName()<<"\n";
        }else{
            glog<<"do not use salt"<<"\n";
        }
    }

    gfb->field_names = field_names;
    gfb->field_types = field_types;
    gfb->field_lengths = field_lengths;

    //then we should read the vector
    std::string prefix = std::string("data/")+db+"/"+table+"/";
    for(unsigned int i=0u; i<gfb->field_names.size(); i++) {
        std::string filename = prefix + gfb->field_names[i];
        std::vector<std::string> column;
        if(IS_NUM(gfb->field_types[i])){
              loadFileNoEscapeLimitCount(filename,column,constGlobalConstants.loadCount);
        }else{
              loadFileEscapeLimitCount(filename,column,gfb->field_lengths[i],constGlobalConstants.loadCount);
        }
        std::reverse(column.begin(),column.end());
        gfb->annoOnionNameToFileVector[gfb->field_names[i]] = std::move(column);
    }
    //init another map
    for(unsigned int i=0;i<gfb->field_names.size();i++){
        gfb->annoOnionNameToType[gfb->field_names[i]] = gfb->field_types[i];
    }
}

/*load file, decrypt, and then return data plain fields in the type ResType*/
static ResType load_files(std::string db, std::string table){
    timer t_load_files;
    std::unique_ptr<SchemaInfo> schema =  myLoadSchemaInfo(gembeddedDir);
    //get all the fields in the tables.
    std::vector<FieldMeta*> fms = getFieldMeta(*schema,db,table);
    TableMetaTrans res_meta = loadTableMetaTrans(db,table);
    std::vector<FieldMetaTrans> res = res_meta.getFts();
    for(unsigned int i=0;i<fms.size();i++){
        res[i].trans(fms[i]);
    }

    glog<<"loadtablemeta: "<<
          std::to_string(t_load_files.lap()/1000000u)<<
          "##"<<std::to_string(time(NULL))<<"\n";
    //then we should load all the fields available
    initGfb(res,db,table);
    glog<<"initGfb: "<<
          std::to_string(t_load_files.lap()/1000000u)<<
          "##"<<std::to_string(time(NULL))<<"\n";

    std::shared_ptr<ReturnMeta> rm = getReturnMeta(fms,res);

    glog<<"getReturnMeta: "<<
          std::to_string(t_load_files.lap()/1000000u)<<
          "##"<<std::to_string(time(NULL))<<"\n";

    vector<string> field_names = ggbt->field_names;
    vector<int> field_types = ggbt->field_types;
    vector<int> field_lengths = ggbt->field_lengths;

    //why do we need this??
    create_embedded_thd(0);
    rawMySQLReturnValue resraw;
    vector<vector<string>> res_field;   
    for(auto item:field_names){
        res_field.push_back(gfb->annoOnionNameToFileVector[item]);
    }
    //check here
    for(auto item:res_field){
        if(item.size()==0) {
            return ResType(false, 0, 0);
        }
    }
    //then transform it to ress_fields
    unsigned int length = res_field[0].size();

    vector<vector<string>> ress_field;
    for(unsigned int i=0u;i<length;i++) {
        vector<string> row;
        for(unsigned int j=0u;j<res_field.size();j++){
            row.push_back(res_field[j][i]);
        }
        ress_field.push_back(row);
    }

    resraw.rowValues = ress_field;
    resraw.fieldNames = field_names;
    for(unsigned int i=0;i<field_types.size();++i){
	resraw.fieldTypes.push_back(static_cast<enum_field_types>(field_types[i]));
    }
    ResType rawtorestype = rawMySQLReturnValue_to_ResType(false, &resraw);

    glog<<"transform: "<<
          std::to_string(t_load_files.lap()/1000000u)<<
          "##"<<std::to_string(time(NULL))<<"\n";

    auto finalresults = decryptResults(rawtorestype,*rm);

    glog<<"descryption: "<<
           std::to_string(t_load_files.lap()/1000000u)<<
           "##"<<std::to_string(time(NULL))<<"\n";
    return finalresults;
}

std::map<onion,unsigned long> gcountMap;

static
void local_wrapper(const Item &i, const FieldMeta &fm, Analysis &a,
                           List<Item> * append_list) {
    //append_list->push_back(&(const_cast<Item&>(i)));
    //do not use the plain strategy 
    std::vector<Item *> l;
    const uint64_t salt = fm.getHasSalt() ? randomValue() : 0;
    uint64_t IV = salt;
    for (auto it : fm.orderedOnionMetas()) {
        if(RiboldMYSQL::is_null(i)){
            l.push_back(RiboldMYSQL::clone_item(i));
            continue;
        }
        const onion o = it.first->getValue();
        OnionMeta * const om = it.second;
        std::string annoOnionName = om->getAnonOnionName();
        if(gfb->annoOnionNameToFileVector.find(annoOnionName)!=gfb->annoOnionNameToFileVector.end()){
            enum_field_types type = static_cast<enum_field_types>(gfb->annoOnionNameToType[annoOnionName]);
            std::vector<std::string> &tempFileVector = gfb->annoOnionNameToFileVector[annoOnionName];
            std::string in = tempFileVector.back();            
            if(IS_NUM(type)){
                unsigned int len = annoOnionName.size();
                if(len>4u&&annoOnionName.substr(len-4)=="ASHE"){
                    l.push_back(MySQLFieldTypeToItem(type,in));
                }else{
                    l.push_back( new (current_thd->mem_root)
                                Item_int(static_cast<ulonglong>(valFromStr(in))) );
                }
            }else{
                l.push_back(MySQLFieldTypeToItem(type,in));
            }
            tempFileVector.pop_back();
        }else{
            l.push_back(my_encrypt_item_layers(i, o, *om, a, IV));
            gcountMap[o]++;
        }
    }
    std::string saltName = fm.getSaltName();
    if (fm.getHasSalt()) {
        if(gfb->annoOnionNameToFileVector.find(saltName)!=gfb->annoOnionNameToFileVector.end()){
            std::vector<std::string> &tempFileVector = gfb->annoOnionNameToFileVector[saltName];
            std::string in = tempFileVector.back();
            l.push_back( new (current_thd->mem_root)
                                Item_int(static_cast<ulonglong>(valFromStr(in)))
             );
            tempFileVector.pop_back();
            gcountMap[oINVALID]++;//use invalid to record the salt hit rate
        }else{
            l.push_back(new Item_int(static_cast<ulonglong>(salt)));
        }
    }

    for (auto it : l) {
        append_list->push_back(it);
    }
}

static
List<Item> * processRow(const std::vector<Item *> &row,
                        const std::vector<std::string> &names,
                        Analysis &analysis,
                        std::string db,
                        std::string table) {
    List<Item> *const newList0 = new List<Item>();
    for(auto i=0u;i<names.size();i++){
        std::string field_name = names[i];
        FieldMeta & fm = analysis.getFieldMeta(db,table,field_name);
        local_wrapper(*row[i],fm,analysis,newList0);
    }
    return newList0;
}


int
main(int argc, char* argv[]){
    timer t_init;
    glog<<"init: "<<
          std::to_string(t_init.lap()/1000000u)<<
          "##"<<std::to_string(time(NULL))<<"\n";
    std::string db="tdb",table="-1";
    std::string ip="localhost";
    if(argc==4){
        ip = std::string(argv[1]);
        db = std::string(argv[2]);
        table = std::string(argv[3]);
    }

    globalConn = globalInit(ip,3306);
    globalConn->execute(std::string("use ")+db);

    create_embedded_thd(0);

    std::unique_ptr<SchemaInfo> schema =  myLoadSchemaInfo(gembeddedDir);
    schema.get();
    const std::unique_ptr<AES_KEY> &TK = std::unique_ptr<AES_KEY>(getKey(std::string("113341234")));
    Analysis analysis(db, *schema, TK, SECURITY_RATING::SENSITIVE);

    glog<<"loadSchema: "<<
          std::to_string(t_init.lap()/1000000u)<<
          "##"<<std::to_string(time(NULL))<<"\n";

    std::vector<std::string> tables;
    if(table==std::string("-1")){
        tables = getDbTables(db);
        std::map<std::string,int> annIndex;
        for(unsigned int i=0u;i<tables.size();++i) {
            annIndex[tables[i]]=i;
        }
        const DatabaseMeta & dbm = analysis.getDatabaseMeta(db);
        auto &tableMetas = dbm.getChildren();
        for(auto & kvtable:tableMetas){
            auto annoname = kvtable.second->getAnonTableName();
            auto plainname = kvtable.first.getValue();
            tables[annIndex[annoname]]=plainname;
        }
    }else{
        tables.push_back(table);
    }
    for(auto &table:tables) {
        gfb = new fullBackUp;
        ggbt = new batch;
        glog<<"====================start table: "<<table<<"============================"<<"\n";
        /*choose decryption onion, load and decrypt to plain text*/
        ResType res =  load_files(db,table);
        if(res.success()) {   
            glog<<"load_files: "<<
                  std::to_string(t_init.lap()/1000000u)<<
                  "##"<<std::to_string(time(NULL))<<"\n";    
            std::string annoTableName = analysis.getTableMeta(db,table).getAnonTableName();
            const std::string head = std::string("INSERT INTO `")+db+"`.`"+annoTableName+"` ";    
            /*reencryption to get the encrypted insert!!!*/
            unsigned int i=0u;
            while(true){
                List<List_item> newList;
                int localCount=0;
                for(;i<res.rows.size();i++){
                    List<Item> * newList0 = processRow(res.rows[i],
                                                       res.names,
                                                       analysis,db,table);
                    newList.push_back(newList0);
                    localCount++;
                    if(localCount==constGlobalConstants.pipelineCount){
                        std::ostringstream o;
                        insertManyValues(o,newList);
                        globalConn->execute(head+o.str());
//                        std::cout<<(head+o.str())<<std::endl;
                        i++;
                        break;
                    }
                }
                if(i>=res.rows.size()){
                    if(localCount!=constGlobalConstants.pipelineCount) {
                        std::ostringstream o;
                        insertManyValues(o,newList);
                        globalConn->execute(head+o.str());
//                        std::cout<<(head+o.str())<<std::endl;
                    }
                    break;
                }
            }
            glog<<"reencryptionAndInsert: "<<
                std::to_string(t_init.lap()/1000000u)<<
                "##"<<std::to_string(time(NULL))<<"\n";    
            for(auto item:gcountMap) {
                glog<<"onionComputed: "<<
                      TypeText<onion>::toText(item.first)<<"::"<<
                      std::to_string(item.second)<<"\n";
            }
        }
        glog<<"====================finish table: "<<table<<"============================"<<"\n";
        gcountMap.clear();
        delete gfb;
        delete ggbt;
    }
    return 0;
}
