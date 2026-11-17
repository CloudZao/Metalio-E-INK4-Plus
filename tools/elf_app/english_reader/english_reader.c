#include "app_util.h"
#include "content.h"
static unsigned mode, chapter, page, toc, word_page, quiz, attempts, correct;
static int answered;
static void* heading;
static void* content;
static void* footer;
static void* result;

static void Show(void) {
    ResetText();
    if (mode == 0) {
        Add(zh?"章节目录":"Chapters"); app->set_text(heading,text); ResetText();
        for (unsigned i=0;i<4 && toc+i<CHAPTER_COUNT;++i) {Number(i+1);Add(". ");Add(chapters[toc+i].title);Add("\n\n");}
        app->set_text(content,text); ResetText();Add(zh?"选择序号进入章节":"Choose a section number");
    } else if (mode == 1) {
        Add(chapters[chapter].title);app->set_text(heading,text);
        app->set_text(content,pages[page]);ResetText();
        Number(page-chapters[chapter].first+1);Add(" / ");Number(chapters[chapter].count);
        Add(zh?" 页  |  1-4 跳转章节位置":" pages | 1-4 jump within section");
    } else if (mode == 2) {
        Add(zh?"词汇卡片":"Word cards");app->set_text(heading,text);ResetText();
        for(unsigned i=0;i<4 && word_page+i<WORD_COUNT;++i){Number(i+1);Add(". ");Add(words[word_page+i].en);Add("\n\n");}
        app->set_text(content,text);ResetText();Number(word_page+1);Add(" / ");Number(WORD_COUNT);
        Add(zh?"  选择序号查看释义":" | Choose a number for meaning");
    } else {
        Add(zh?"选择对应的中文释义":"Choose the Chinese meaning");app->set_text(heading,text);ResetText();
        const unsigned item=quiz*7%WORD_COUNT, answer=quiz%4;
        Add(words[item].en);Add("\n\n");
        for(unsigned i=0;i<4;++i){Number(i+1);Add(". ");Add(words[(item+(i+4-answer)%4*17)%WORD_COUNT].zh);Add("\n\n");}
        app->set_text(content,text);ResetText();Add(zh?"答对 ":"Correct ");Number(correct);Add(" / ");Number(attempts);
    }
    app->set_text(footer,text);
}
static void Click(void* context) {
    const unsigned key=(unsigned)(uintptr_t)context;
    if(key>=100 && key<104){mode=key-100;answered=0;app->set_text(result,"");Show();return;}
    if(key==20 || key==21) {
        const int next=key==21;
        if(mode==0){if(next && toc+4<CHAPTER_COUNT)toc+=4;else if(!next && toc>=4)toc-=4;}
        else if(mode==1){if(next && page+1<chapters[chapter].first+chapters[chapter].count)++page;else if(!next && page>chapters[chapter].first)--page;}
        else if(mode==2){if(next && word_page+4<WORD_COUNT)word_page+=4;else if(!next && word_page>=4)word_page-=4;}
        else {if(next)++quiz;else if(quiz)--quiz;answered=0;}
        app->set_text(result,"");Show();return;
    }
    if(key>3)return;
    if(mode==0) {if(toc+key>=CHAPTER_COUNT)return;chapter=toc+key;page=chapters[chapter].first;mode=1;app->set_text(result,"");Show();}
    else if(mode==1){page=chapters[chapter].first+(chapters[chapter].count-1)*key/3;Show();}
    else if(mode==2){if(word_page+key>=WORD_COUNT)return;ResetText();Add(words[word_page+key].en);Add("  ");Add(words[word_page+key].zh);app->set_text(result,text);}
    else {
        if(answered)return;
        answered=1;++attempts;
        const int right=key==quiz%4;if(right)++correct;
        ResetText();Add(right?(zh?"回答正确":"Correct answer"):(zh?"正确释义：":"Correct meaning: "));
        if(!right)Add(words[quiz*7%WORD_COUNT].zh);
        app->set_text(result,text);Show();
    }
}
int app_main(int argc,char** argv) {
    if(!AppInit(argc,argv))return -1;
    const int pad=18,gap=10,w=(app->width-pad*2-gap*3)/4;
    static const char* const cn[]={"目录","阅读","词汇","测验"};
    static const char* const en[]={"Chapters","Read","Words","Quiz"};
    for(int i=0;i<4;++i)Button(pad+i*(w+gap),10,w,64,zh?cn[i]:en[i],Click,100+i);
    heading=Label(pad,92,app->width-pad*2,72,"");
    content=Label(pad,180,app->width-pad*2,app->height-464,"");
    for(int i=0;i<4;++i){char number[2]={(char)('1'+i),0};Button(pad+i*(w+gap),app->height-266,w,64,number,Click,i);}
    result=Label(pad,app->height-188,app->width-pad*2,64,"");
    footer=Label(pad,app->height-120,app->width-pad*2,50,"");
    const int nav=(app->width-pad*2-gap)/2;
    Button(pad,app->height-64,nav,56,zh?"上一页":"Previous",Click,20);
    Button(pad+nav+gap,app->height-64,nav,56,zh?"下一页":"Next",Click,21);
    mode=1;chapter=0;page=0;Show();return 0;
}
