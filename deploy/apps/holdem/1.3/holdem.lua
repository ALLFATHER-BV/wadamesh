local TIT_T,SUB_T="TEXAS HOLD'EM","No-Limit 4-Max  ·  3 AI Opponents"
-- Texas Hold'em Poker: WADAMESH Lua app, landscape and portrait, from 240 px wide screens up to the T-Display P4
local ui,sys,store,timer=wada.ui,wada.sys,wada.store,wada.timer
local C,fmt,flr,max,min,abs=ui.colors,string.format,math.floor,math.max,math.min,math.abs
local app={}
local W,H,cv=320,208,nil
local dname,port,s2="T-Deck",false,false
local top_h,bot_h,tbl_h,cc_w,cc_h,cc_g,p_cw,p_ch=20,30,158,37,48,5,37,48
local p_sm,p_lg,f_xs,f_sm,f_md,f_lg,f_xl,f_rk=3,5,10,11,12,14,16,12
local lh_xs,lh_sm,lh_md,lh_lg,lh_xl,pad,gap=14,15,16,18,20,6,4
local K_FLT,K_FDK,K_FLN,K_BAR=0x0B2B1E,0x082016,0x164A35,0x0E1318
local K_POD,K_PFD,K_CBG,K_CSH=0x101820,0x0A1214,0xF5F7FA,0x06150E
local K_CBK,K_CTR,K_RED,K_BLK,K_GLD=0x1B4F8A,0x4A8FE7,0xD9383A,0x181C24,0xF2C14E
local K_BTN,K_BAC,K_BDS,K_DIM=0x1A222B,0x1E3A3A,0x11161C,0x525A65
local S0=500
local BLINDS={{5,10},{10,20},{25,50}}
local b_idx=1
local RANKS={"A","2","3","4","5","6","7","8","9","10","J","Q","K"}
local HNAMES={[0]="HIGH CARD","ONE PAIR","TWO PAIR","3 OF A KIND","STRAIGHT","FLUSH","FULL HOUSE","4 OF A KIND","STR. FLUSH","ROYAL FLUSH"}
local SNAMES={[0]="PRE-FLOP","FLOP","TURN","RIVER","SHOWDOWN"}
local best_b,h_won,h_play,snd=S0,0,0,true
local state,street,show_all="title",0,false
local deck,board,btns={},{},{}
local d_top,pot,c_bet,min_r,r_to=0,0,0,10,20
local dlr_s,trn_s,f_idx=1,1,1
local b_tit,b_sub,b_col="","",0x15B6A6
local PL={}
local seats={}
for i,n in ipairs({"YOU","TEX","VIPER","DUKE"})do
seats[i]={name=n,hum=(i==1),sty=({"h","l","t","g"})[i],stack=S0,cards={},sb=0,tb=0,fold=false,all=false,act=false,st="",role=""}
end
local function I(x)return flr(tonumber(x)or 0)end
local function clp(v,l,h)return v<l and l or(v>h and h or v)end
local function sfx()if snd and sys.beep then pcall(sys.beep)end end
local function rint(l,h)
l,h=I(l),I(h);if h<=l then return l end
local r=sys.random(l,h)
return I((r<l or r>h)and(l+(abs(r)%(h-l+1)))or r)
end
local function tw(s,z)s,z=tostring(s or""),I(z or f_sm);return ui.text_w and I(ui.text_w(s,z))or I(#s*(z+4)*0.48)end
local function th(z)z=I(z or f_sm);return ui.text_h and I(ui.text_h(z))or(z+4)end
local function tcx(x,y,s,c,z)cv:text(I(x-tw(s,z)/2),y,s,c,z)end
local function box(x,y,w,h,bg,fg,r)cv:rect(x,y,w,h,bg,true,r);if fg then cv:rect(x,y,w,h,fg,false,r)end end
local function layout()
local s,l=min(W,H),max(W,H);port=(H>W)
if l>=1185 or(W==568 and H>=900)or(W>=1185 and H>=460)then dname,s2="T-Display P4 AMOLED",true
elseif(l>=1050 and l<1185)or(W==540 and H>=900)or(W>=500 and W<560 and H>=900)then dname,s2="T-Display P4 LCD",true
elseif l>=700 or s>=460 then dname,s2="Large HD",true
else dname,s2="T-Deck",false end
if s2 then f_xs,f_sm,f_md,f_lg,f_xl,f_rk=14,14,16,20,24,16 else f_xs,f_sm,f_md,f_lg,f_xl,f_rk=10,11,12,14,16,12 end
lh_xs,lh_sm,lh_md,lh_lg,lh_xl=th(f_xs),th(f_sm),th(f_md),th(f_lg),th(f_xl)
if s2 then top_h,bot_h,pad,gap=I(max(lh_md+16,40)),I(max(lh_md+26,54)),14,8
else top_h,bot_h,pad,gap=20,30,6,4 end
tbl_h=max(80,H-top_h-bot_h)
if s2 then
cc_h=I(clp(tbl_h*0.3,88,120));cc_w=I(cc_h*0.74);cc_g,p_cw,p_ch,p_sm,p_lg=12,cc_w,cc_h,5,11
else
cc_h=clp(I(tbl_h*0.3),40,48);cc_w=clp(I(cc_h*0.77),32,37);cc_g,p_cw,p_ch,p_sm,p_lg=5,cc_w,cc_h,3,5
end
if port then
-- Portrait: opponents as full-width rows on top, then the board, then your cards and
-- an info box, then two rows of buttons. Sized from W and H so 240 x 289 fits and a
-- tall panel scales the cards up; spare height goes to the rows, gaps and a centre offset.
local hp,vg,pv,hg=s2 and 16 or 6,s2 and 12 or 3,s2 and 8 or 3,s2 and 10 or 4
local brh=s2 and I(max(lh_md+20,52))or I(max(lh_sm+7,24))
bot_h=brh*2+pv*3;tbl_h=max(80,H-top_h-bot_h)
local oph,og=lh_xs*2+(s2 and 10 or-2),s2 and 8 or 2
cc_g=s2 and 10 or 4
cc_w=I(clp((W-hp*2-cc_g*4)/5,24,s2 and 130 or 64));cc_h=I(cc_w*1.34)
local av=tbl_h-oph*3-og*2-vg*4
if cc_h*2>av then cc_h=max(30,I(av/2));cc_w=min(cc_w,I(cc_h*0.74));p_cw,p_ch=cc_w,cc_h
else p_ch=min(I(cc_h*1.4),av-cc_h);p_cw=min(I(p_ch*0.74),I((W*0.58-hp*2-hg)/2));p_ch=min(p_ch,I(p_cw*1.36))end
local sp=max(0,av-cc_h-p_ch);local go=min(I(sp*0.4/3),oph);oph,sp=oph+go,sp-go*3
local ex=min(I(sp/4),s2 and 24 or 8)
p_sm,p_lg=I(clp(p_cw*0.06,3,6)),I(clp(p_cw*0.11,5,14))
PL={hp=hp,vg=vg+ex,pv=pv,hg=hg,oph=oph,og=og,y0=I((sp-ex*4)/2)}
PL.mch=oph-(s2 and 10 or 4);PL.mcw=I(PL.mch*0.72)
end
end
local function make_cv(w,h)
w,h=I(w),I(h)
if w<=480 and h<=480 then local s=ui.canvas(w,h);s:pos(0,0);return s end
local nx,ny=math.ceil(w/480),math.ceil(h/480)
local bw,bh,ts=I(math.ceil(w/nx)),I(math.ceil(h/ny)),{}
for ry=0,ny-1 do
local ty,th_t=ry*bh,min(bh,h-ry*bh)
for cx=0,nx-1 do
local tx,tw_t=cx*bw,min(bw,w-cx*bw)
if tw_t>0 and th_t>0 then local tc=ui.canvas(tw_t,th_t);tc:pos(tx,ty);ts[#ts+1]={c=tc,x=tx,y=ty,x2=tx+tw_t,y2=ty+th_t}end
end
end
local v={tiles=ts}
function v:fill(c)c=I(c);for i=1,#ts do ts[i].c:fill(c)end end
function v:rect(x,y,rw,rh,c,f,r)
x,y,rw,rh,c=x-(x%1),y-(y%1),rw-(rw%1),rh-(rh%1),c-(c%1)
local rd,x2,y2=r~=nil and(r-(r%1))or nil,x+rw,y+rh
for i=1,#ts do
local t=ts[i]
if x2>t.x and x<t.x2 and y2>t.y and y<t.y2 then
if rd~=nil then t.c:rect(x-t.x,y-t.y,rw,rh,c,f,rd)else t.c:rect(x-t.x,y-t.y,rw,rh,c,f)end
end
end
end
function v:line(x1,y1,x2,y2,c,lw)
x1,y1,x2,y2,c,lw=I(x1),I(y1),I(x2),I(y2),I(c),lw and I(lw)or 1
local lx1,ly1,lx2,ly2=min(x1,x2)-lw,min(y1,y2)-lw,max(x1,x2)+lw+1,max(y1,y2)+lw+1
for i=1,#ts do
local t=ts[i]
if lx2>t.x and lx1<t.x2 and ly2>t.y and ly1<t.y2 then t.c:line(x1-t.x,y1-t.y,x2-t.x,y2-t.y,c,lw)end
end
end
local function seg(x,y,s,c,z)
x,y,c,z=I(x),I(y),I(c),I(z or f_sm)
local sw,sh=tw(s,z),th(z)
if sw>bw-10 then
local m,sp=flr(#s/2),nil
for d=0,m-1 do
if s:sub(m+d,m+d)==" "then sp=m+d;break end
if m-d>=1 and s:sub(m-d,m-d)==" "then sp=m-d;break end
end
if sp and sp>1 and sp<#s then local ls=s:sub(1,sp-1);seg(x,y,ls,c,z);seg(x+tw(ls.." ",z),y,s:sub(sp+1),c,z);return end
end
for i=1,#ts do
local t=ts[i]
if x+sw+4>t.x and x<t.x2 and y+sh+4>t.y and y<t.y2 then t.c:text(x-t.x,y-t.y,s,c,z)end
end
end
function v:text(x,y,s,c,z)seg(x,y,tostring(s or""),c,z)end
return v
end
local function blinds()local b=BLINDS[b_idx]or BLINDS[1];return b[1],b[2]end
local function save_stats()
local ps=I(seats[1].stack);if ps>best_b then best_b=ps end
store.set("he_bank",ps);store.set("he_best",I(best_b));store.set("he_wins",I(h_won));store.set("he_hands",I(h_play));store.set("he_blind",I(b_idx))
if wada.widget and wada.widget.set then
local pct=h_play>0 and I((h_won*100)/h_play)or 0
pcall(wada.widget.set,{title="Hold'em",value=fmt("$%d",ps),line=fmt("Peak $%d  ·  Won %d%% (%d/%d)",I(best_b),pct,I(h_won),I(h_play))})
end
end
local function build_deck()
if#deck~=52 then deck={};local k=1;for s=1,4 do for r=1,13 do deck[k]={r=r,s=s};k=k+1 end end end
for i=52,2,-1 do local j=rint(1,i);deck[i],deck[j]=deck[j],deck[i]end
d_top=52
end
local function dcard()if d_top<1 then build_deck()end;local c=deck[d_top];d_top=d_top-1;return c end
local function pval(r)return r==1 and 14 or r end
local function eval_hand(hole,comm)
local all={}
for i=1,#hole do all[#all+1]=hole[i]end
if comm then for i=1,#comm do all[#all+1]=comm[i]end end
if#all==0 then return 0,0,"HIGH CARD"end
local rc,sc,sb,rb={},{0,0,0,0},{0,0,0,0},0
for v=2,14 do rc[v]=0 end
for i=1,#all do
local v,s=pval(all[i].r),all[i].s
rc[v],sc[s]=rc[v]+1,sc[s]+1
local b=1<<v;rb,sb[s]=rb|b,sb[s]|b
if v==14 then rb,sb[s]=rb|2,sb[s]|2 end
end
local function pk(c,k1,k2,k3,k4,k5)return I(c*1000000+(k1 or 0)*38416+(k2 or 0)*2744+(k3 or 0)*196+(k4 or 0)*14+(k5 or 0))end
local function tst(m)for hi=14,5,-1 do local k=31<<(hi-4);if(m&k)==k then return hi end end;return 0 end
local fs=0;for s=1,4 do if sc[s]>=5 then fs=s;break end end
if fs>0 then local sf=tst(sb[fs]);if sf>0 then local c=(sf==14)and 9 or 8;return pk(c,sf),c,HNAMES[c]end end
local qd,tr,pr,sg={},{},{},{}
for v=14,2,-1 do
local c=rc[v]
if c==4 then qd[#qd+1]=v elseif c==3 then tr[#tr+1]=v elseif c==2 then pr[#pr+1]=v elseif c==1 then sg[#sg+1]=v end
end
if#qd>0 then
local q,k=qd[1],0;for v=14,2,-1 do if v~=q and rc[v]>0 then k=v;break end end
return pk(7,q,k),7,HNAMES[7]
end
if#tr>0 and(#tr>=2 or#pr>=1)then return pk(6,tr[1],(#tr>=2)and tr[2]or pr[1]),6,HNAMES[6]end
if fs>0 then
local fc,m={},sb[fs]
for v=14,2,-1 do if(m&(1<<v))~=0 then fc[#fc+1]=v;if#fc==5 then break end end end
return pk(5,fc[1],fc[2],fc[3],fc[4],fc[5]),5,HNAMES[5]
end
local st=tst(rb);if st>0 then return pk(4,st),4,HNAMES[4]end
if#tr>0 then
local t,k1,k2=tr[1],0,0
for v=14,2,-1 do if v~=t and rc[v]>0 then if k1==0 then k1=v elseif k2==0 then k2=v;break end end end
return pk(3,t,k1,k2),3,HNAMES[3]
end
if#pr>=2 then
local p1,p2,k1=pr[1],pr[2],0;for v=14,2,-1 do if v~=p1 and v~=p2 and rc[v]>0 then k1=v;break end end
return pk(2,p1,p2,k1),2,HNAMES[2]
end
if#pr==1 then return pk(1,pr[1],sg[1],sg[2],sg[3]),1,HNAMES[1]end
return pk(0,sg[1],sg[2],sg[3],sg[4],sg[5]),0,HNAMES[0]
end
local rebuild_btns,draw_screen,adv_turn,next_street,resolve
local function cnt_pl()
local a,c=0,0
for i=1,4 do if not seats[i].fold then a=a+1;if not seats[i].all and seats[i].stack>0 then c=c+1 end end end
return a,c
end
local function post(i,amt)
local p=seats[i];local a=I(min(amt,p.stack))
p.stack,p.sb,p.tb,pot=I(p.stack-a),I(p.sb+a),I(p.tb+a),I(pot+a)
if p.stack<=0 then p.stack,p.all=0,true end
return a
end
local function sync_r()
local _,bb=blinds()
local mt=I(seats[1].sb+seats[1].stack)
local mn=(c_bet==0)and bb or I(c_bet+max(bb,min_r))
r_to=I(clp(r_to,min(mn,mt),mt))
end
local function act_pl(idx,kind,tgt)
local p=seats[idx];local _,bb=blinds();p.act=true
if kind=="fold"then p.fold,p.st=true,"FOLD";sfx()
elseif kind=="call"then
local nd=I(c_bet-p.sb)
if nd<=0 then p.st="CHECK"else post(idx,nd);p.st=p.all and"ALL-IN"or fmt("CALL $%d",I(p.sb))end;sfx()
else
local mt=I(p.sb+p.stack)
local ds=(kind=="allin")and mt or I(clp(tgt or r_to,c_bet+bb,mt))
local ad=I(ds-p.sb);if ad>0 then post(idx,ad)end
if p.sb>c_bet then
local df=I(p.sb-c_bet);if df>min_r then min_r=df end;c_bet=p.sb
for i=1,4 do if i~=idx and not seats[i].fold and not seats[i].all then seats[i].act=false end end
end
p.st=p.all and fmt("ALL-IN $%d",I(p.sb))or((c_bet==ad)and fmt("BET $%d",I(p.sb))or fmt("RAISE $%d",I(p.sb)));sfx()
end
if cnt_pl()<=1 then resolve(true);return end
adv_turn()
end
local function ai_go(idx)
local p=seats[idx];local _,bb=blinds();local tc=I(c_bet-p.sb)
local sc,cat=eval_hand(p.cards,board);local str=25
if street==0 then
local v1,v2=pval(p.cards[1].r),pval(p.cards[2].r);local hi,lo=max(v1,v2),min(v1,v2)
str=(v1==v2)and(52+hi*3)or((hi*2)+lo+((p.cards[1].s==p.cards[2].s)and 8 or 0)+((hi-lo==1)and 5 or 0))
else str=18+cat*18+I((sc%1000000)/38416)end
local rl=rint(1,100)
if p.sty=="l"then str=str+10 elseif p.sty=="g"then str=str+(rl>65 and 18 or 4)elseif p.sty=="t"then str=str+(street>0 and cat>=1 and 8 or-3)end
if tc<=0 then
if str>=58 and p.stack>bb and rl<=70 then act_pl(idx,"raise",I(min(p.sb+p.stack,max(bb,I(pot*0.5)))))else act_pl(idx,"call")end
else
local ef=str-I((tc*45)/max(bb,pot))
if tc>=p.stack then act_pl(idx,(str>=52 or(tc<=bb*2 and str>=36))and"allin"or"fold")
elseif ef>=54 and p.stack>tc+bb*2 and rl<=55 then act_pl(idx,"raise",I(c_bet+max(min_r,bb*2)))
elseif ef>=24 or tc<=bb then act_pl(idx,"call")else act_pl(idx,"fold")end
end
end
adv_turn=function()
local act,can=cnt_pl();if act<=1 then resolve(true);return end
for stp=1,4 do
local nxt=((trn_s+stp-1)%4)+1;local p=seats[nxt]
if not p.fold and not p.all and p.stack>0 and(not p.act or p.sb<c_bet)then
if can==1 and p.sb>=c_bet then break end
trn_s=nxt
if p.hum then timer.stop();state="player";sync_r();rebuild_btns();draw_screen()
else state="ai";rebuild_btns();draw_screen();timer.every(3780)end
return
end
end
next_street()
end
next_street=function()
timer.stop()
for i=1,4 do seats[i].sb,seats[i].act=0,false;if not seats[i].fold and not seats[i].all then seats[i].st=""end end
local _,bb=blinds();c_bet,min_r=0,bb
if street==0 then street=1;board[1],board[2],board[3]=dcard(),dcard(),dcard()
elseif street==1 then street=2;board[4]=dcard()
elseif street==2 then street=3;board[5]=dcard()
else resolve(false);return end
sfx();local _,can=cnt_pl()
if can<2 then show_all,state=true,"ai";rebuild_btns();draw_screen();timer.every(520);return end
trn_s=dlr_s;adv_turn()
end
resolve=function(by_fold)
timer.stop();state,street="done",4
if by_fold then
local w=1;for i=1,4 do if not seats[i].fold then w=i;break end end
seats[w].stack,seats[w].st,h_play=I(seats[w].stack+pot),"WIN!",h_play+1
if w==1 then h_won=h_won+1;b_tit,b_sub,b_col="YOU WIN POT!",fmt("+$%d (Opponents Folded)",I(pot)),C.good
else b_tit,b_sub,b_col=seats[w].name.." WINS",fmt("Pot $%d (Folded)",I(pot)),C.accent end
pot=0;save_stats();rebuild_btns();draw_screen();return
end
show_all=true;local scs,hns,bw,bs={},{},1,-1
for i=1,4 do
if not seats[i].fold then
local sc,_,nm=eval_hand(seats[i].cards,board)
scs[i],hns[i],seats[i].st=sc,nm,nm;if sc>bs then bs,bw=sc,i end
else scs[i]=-1 end
end
local rem,won={seats[1].tb,seats[2].tb,seats[3].tb,seats[4].tb},{0,0,0,0}
while true do
local ml=999999
for i=1,4 do if not seats[i].fold and rem[i]>0 and rem[i]<ml then ml=rem[i]end end
if ml==999999 then break end
local sp,el=0,{}
for i=1,4 do
local tk=min(rem[i],ml);if tk>0 then rem[i],sp=rem[i]-tk,sp+tk end
if not seats[i].fold and tk==ml then el[#el+1]=i end
end
if sp>0 and#el>0 then
local ms,ws=-1,{}
for _,idx in ipairs(el)do if scs[idx]>ms then ms=scs[idx]end end
for _,idx in ipairs(el)do if scs[idx]==ms then ws[#ws+1]=idx end end
local sh=I(sp/#ws);local od=I(sp-sh*#ws)
for wi,idx in ipairs(ws)do local aw=sh+(wi==1 and od or 0);seats[idx].stack,won[idx]=I(seats[idx].stack+aw),I(won[idx]+aw)end
end
local any=false;for i=1,4 do if not seats[i].fold and rem[i]>0 then any=true end end
if not any then for i=1,4 do if rem[i]>0 then seats[i].stack,rem[i]=I(seats[i].stack+rem[i]),0 end end;break end
end
h_play=h_play+1
if won[1]>0 then h_won=h_won+1;b_tit,b_sub,b_col=fmt("YOU WIN $%d!",I(won[1])),hns[1]or"BEST HAND",K_GLD
else b_tit,b_sub,b_col=fmt("%s WINS $%d",seats[bw].name,I(won[bw])),hns[bw]or"SHOWDOWN",seats[1].fold and C.accent or C.bad end
pot=0;save_stats();sfx();rebuild_btns();draw_screen()
end
local function start_hand()
local sb,bb=blinds()
if seats[1].stack<bb then seats[1].stack=S0;sys.toast("Free $500 bailout buy-in!")end
for i=2,4 do if seats[i].stack<bb then seats[i].stack=S0;sys.toast(seats[i].name.." re-bought $500")end end
build_deck();board,pot,street,show_all,b_tit,b_sub={},0,0,false,"",""
dlr_s=(dlr_s%4)+1;local sb_s=(dlr_s%4)+1;local bb_s=(sb_s%4)+1
for i=1,4 do
local p=seats[i]
p.cards,p.sb,p.tb,p.fold,p.all,p.act,p.st={dcard(),dcard()},0,0,false,false,false,""
p.role=(i==dlr_s)and"D"or((i==sb_s)and"SB"or((i==bb_s)and"BB"or""))
end
post(sb_s,sb);seats[sb_s].st=fmt("SB $%d",I(seats[sb_s].sb))
post(bb_s,bb);seats[bb_s].st=fmt("BB $%d",I(seats[bb_s].sb))
c_bet,min_r,r_to,trn_s=bb,bb,bb*2,bb_s;sfx();adv_turn()
end
local function adj_r(d)if state=="player"then r_to=I(r_to+d);sync_r();sfx();rebuild_btns();draw_screen()end end
local function cyc_bl()b_idx=(b_idx%#BLINDS)+1;local sb,bb=blinds();sys.toast(fmt("Blinds: $%d / $%d",sb,bb));sfx();save_stats();rebuild_btns();draw_screen()end
local function rst_tbl()for i=1,4 do seats[i].stack=S0 end;sys.toast("New $500 Table");sfx();start_hand()end
rebuild_btns=function()
btns={};local sb,bb=blinds();local cmp=(W<280)
if port then
if state=="title"or state=="done"then
btns={
{label=(state=="title")and"DEAL HOLD'EM [D]"or"NEXT HAND [D]",w=120,primary=true,enabled=true,fn=start_hand,row=1},
{label=fmt("$%d/$%d [B]",sb,bb),w=60,enabled=true,fn=cyc_bl,row=2},
{label=(state=="title")and"RESET $500"or"NEW TABLE [N]",w=60,enabled=true,fn=rst_tbl,row=2},
};f_idx=1
elseif state=="player"then
local p=seats[1];local tc=I(max(0,c_bet-p.sb));local cr=(p.stack>tc);sync_r()
local cl=(tc<=0)and"CHECK"or((tc>=p.stack)and fmt("ALL-IN $%d",I(p.stack))or fmt("CALL $%d",tc))
local rl=(c_bet==0)and fmt("BET $%d",I(r_to))or fmt("RAISE $%d",I(r_to))
btns={
{label=cl,w=96,primary=true,enabled=true,fn=function()act_pl(1,"call")end,row=1},
{label=rl,w=96,enabled=cr,fn=function()act_pl(1,"raise",r_to)end,row=1},
{label="FOLD",w=50,enabled=true,fn=function()act_pl(1,"fold")end,row=2},
{label="-BB",w=40,enabled=cr,fn=function()adj_r(-bb)end,row=2},
{label="+BB",w=40,enabled=cr,fn=function()
local mt=I(p.sb+p.stack);r_to=(r_to>=mt)and I(c_bet+max(bb,min_r))or I(r_to+bb)
sync_r();sfx();rebuild_btns();draw_screen()
end,row=2},
{label="ALL-IN",w=60,enabled=p.stack>0,fn=function()act_pl(1,"allin")end,row=2},
};f_idx=1
elseif state=="ai"then
local who=seats[trn_s]and seats[trn_s].name or"DEALER"
btns={{label=who.." ACTING... SKIP",w=200,enabled=true,fn=function()if state=="ai"then app.on_tick(0)end end,row=1}};f_idx=1
end
return
end
if state=="title"then
btns={
{label=fmt("BLINDS $%d/$%d",sb,bb),w=96,enabled=true,fn=cyc_bl},
{label="RESET $500",w=84,enabled=true,fn=rst_tbl},
{label=cmp and"DEAL"or"DEAL HOLD'EM [D]",w=122,primary=true,enabled=true,fn=start_hand},
};f_idx=3
elseif state=="player"then
local p=seats[1];local tc=I(max(0,c_bet-p.sb));local cr=(p.stack>tc);sync_r()
local cl=(tc<=0)and"CHECK"or((tc>=p.stack)and fmt("ALL-IN $%d",I(p.stack))or fmt("CALL $%d",tc))
local rl=(c_bet==0)and fmt("BET $%d",I(r_to))or fmt("RAISE $%d",I(r_to))
btns={
{label="FOLD",w=44,enabled=true,fn=function()act_pl(1,"fold")end},
{label=cl,w=74,primary=true,enabled=true,fn=function()act_pl(1,"call")end},
{label=rl,w=96,enabled=cr,fn=function()act_pl(1,"raise",r_to)end},
{label="+BB",w=36,enabled=cr,fn=function()
local mt=I(p.sb+p.stack);r_to=(r_to>=mt)and I(c_bet+max(bb,min_r))or I(r_to+bb)
sync_r();sfx();rebuild_btns();draw_screen()
end},
{label="ALL-IN",w=52,enabled=p.stack>0,fn=function()act_pl(1,"allin")end},
};f_idx=2
elseif state=="ai"then
local who=seats[trn_s]and seats[trn_s].name or"DEALER"
btns={{label=who.." ACTING... (TAP TO SKIP)",w=300,enabled=true,fn=function()if state=="ai"then app.on_tick(0)end end}};f_idx=1
elseif state=="done"then
btns={
{label=fmt("$%d/$%d [B]",sb,bb),w=88,enabled=true,fn=cyc_bl},
{label=cmp and"RESET"or"NEW TBL [N]",w=94,enabled=true,fn=rst_tbl},
{label=cmp and"DEAL [D]"or"NEXT HAND [D]",w=120,primary=true,enabled=true,fn=start_hand},
};f_idx=3
end
end
local function disk(tc,cx,cy,r,c)
r=max(1,I(r));local k=max(1,I(r*0.68))
tc:rect(cx-k,cy-r,k*2+1,r*2+1,c,true);tc:rect(cx-r,cy-k,r*2+1,k*2+1,c,true)
end
local P2={
{0,-2,1,1,-1,-1,3,1,-2,0,5,2,0,2,1,2},{-2,-2,2,1,1,-2,2,1,-2,-1,5,2,-1,1,3,1,0,2,1,1},
{0,-2,1,1,-1,-1,3,1,-2,0,5,1,-1,1,3,1,0,2,1,1},{-1,-2,3,1,0,-1,1,1,-2,0,5,2,0,2,1,2},
}
local P4={
{0,-3,1,1,-1,-2,3,1,-2,-1,5,1,-3,0,7,2,0,2,1,1,-1,3,3,1},
{-2,-3,2,1,1,-3,2,1,-3,-2,3,1,1,-2,3,1,-3,-1,7,2,-2,1,5,1,-1,2,3,1,0,3,1,1},
{0,-3,1,1,-1,-2,3,1,-2,-1,5,1,-3,0,7,1,-2,1,5,1,-1,2,3,1,0,3,1,1},
{-1,-3,3,2,-3,-1,2,2,2,-1,2,2,-1,-1,3,2,0,1,1,2,-1,3,3,1},
}
local P5={
{0,-5,1,1,-1,-4,3,1,-2,-3,5,1,-3,-2,7,1,-4,-1,9,1,-5,0,11,2,-4,2,3,1,0,2,1,1,2,2,3,1,-1,3,3,1,-2,4,5,1,-3,5,7,1},
{-3,-5,2,1,2,-5,2,1,-4,-4,4,1,1,-4,4,1,-5,-3,11,3,-4,0,9,1,-3,1,7,1,-2,2,5,1,-1,3,3,1,0,4,1,2},
{0,-5,1,1,-1,-4,3,1,-2,-3,5,2,-3,-1,7,1,-4,0,9,1,-3,1,7,1,-2,2,5,2,-1,4,3,1,0,5,1,1},
{-1,-5,3,1,-2,-4,5,2,-1,-2,3,1,-4,-1,3,1,0,-1,1,1,2,-1,3,1,-5,0,11,2,-4,2,3,1,0,2,1,1,2,2,3,1,-1,3,3,2,-3,5,7,1},
}
local function pip(x,y,s,r)
x,y,r=I(x),I(y),max(2,I(r))
local c=(s==2 or s==3)and K_RED or K_BLK
local tc,ox,oy,ts=cv,0,0,cv.tiles
if ts then
local x1,y1,x2,y2=x-r-2,y-r-2,x+r+2,y+r+4
for i=1,#ts do local t=ts[i];if x1>=t.x and x2<=t.x2 and y1>=t.y and y2<=t.y2 then tc,ox,oy=t.c,t.x,t.y;break end end
end
local px,py=x-ox,y-oy
local tb=(r<=2)and P2 or((r<=4)and P4 or((r==5)and P5 or nil))
if tb then local d=tb[s]or tb[4];for i=1,#d,4 do tc:rect(px+d[i],py+d[i+1],d[i+2],d[i+3],c,true)end;return end
if s==1 then
local bt=I(r*0.25);local sp=max(1,r+bt)
for dy=-r,bt,2 do local hw=I(((dy+r)*r)/sp);tc:rect(px-hw,py+dy,hw*2+1,2,c,true)end
local lr,lx,ly=max(2,I(r*0.36)),max(2,I(r*0.42)),I(r*0.22)
disk(tc,px-lx,py+ly,lr,c);disk(tc,px+lx,py+ly,lr,c)
local sw,st,bw=max(1,I(r*0.22)*2+1),I(r*0.25),max(2,I(r*0.55))
tc:rect(px-I(sw/2),py+st,sw,max(2,r-st+1),c,true);tc:rect(px-bw,py+r,bw*2+1,max(1,I(r*0.18)),c,true)
elseif s==2 then
local lr,lx,ly,td=max(2,I(r*0.46)),max(2,I(r*0.46)),I(r*0.32),-I(r*0.18)
disk(tc,px-lx,py-ly,lr,c);disk(tc,px+lx,py-ly,lr,c)
local sp=max(1,r-td)
for dy=td,r,2 do local hw=max(0,I(((r-dy)*r)/sp));tc:rect(px-hw,py+dy,hw*2+1,2,c,true)end
elseif s==3 then
for dy=-r,r,2 do local hw=max(0,I((r-abs(dy))*0.85));tc:rect(px-hw,py+dy,hw*2+1,2,c,true)end
else
local lr,lx,ty,by=max(2,I(r*0.38)),max(2,I(r*0.48)),I(r*0.48),I(r*0.14)
disk(tc,px,py-ty,lr,c);disk(tc,px-lx,py+by,lr,c);disk(tc,px+lx,py+by,lr,c)
local sw,bw=max(1,I(r*0.22)*2+1),max(2,I(r*0.52))
tc:rect(px-I(sw/2),py-I(r*0.1),sw,max(2,r+I(r*0.1)+1),c,true);tc:rect(px-bw,py+r,bw*2+1,max(1,I(r*0.18)),c,true)
end
end
local function cbox(x,y,cw,ch,cd,hid,smo,lgo)
x,y,cw,ch=I(x),I(y),I(cw),I(ch)
local rd,sh=cw>=64 and 8 or 3,cw>=64 and 3 or 1
cv:rect(x-1,y+sh,cw+sh+1,ch,K_CSH,true,rd)
if hid or not cd then
local p1,p2=cw>=64 and 3 or 1,cw>=64 and 6 or 3
cv:rect(x,y,cw,ch,K_CBG,true,rd);cv:rect(x+p1,y+p1,cw-p1*2,ch-p1*2,K_CBK,true,max(2,rd-1))
if cw>=26 then
cv:rect(x+p2,y+p2,cw-p2*2,ch-p2*2,K_CTR,false,max(2,rd-2))
local ew,eh=max(6,I(cw*0.28)),max(6,I(ch*0.22))
cv:rect(I(x+(cw-ew)/2),I(y+(ch-eh)/2),ew,eh,K_GLD,false,2)
end;return
end
box(x,y,cw,ch,K_CBG,0x64748B,rd)
local c,rs=(cd.s==2 or cd.s==3)and K_RED or K_BLK,RANKS[cd.r]
if cw<=24 then cv:text(x+2,y+1,rs,c,10);pip(x+I(cw/2),y+ch-7,cd.s,2)
elseif cw<64 then
cv:text(x+3,y+2,rs,c,12);if cw>=32 then pip(x+cw-7,y+8,cd.s,smo or 3)end
pip(x+I(cw/2),y+I(ch/2)+5,cd.s,lgo or 5)
else
local sm,lg=smo or p_sm,lgo or p_lg
cv:text(x+6,y+4,rs,c,f_rk);pip(x+cw-sm-7,y+sm+6,cd.s,sm)
if cw>=72 and ch>=96 then cv:text(x+cw-6-tw(rs,f_rk),y+ch-4-th(f_rk),rs,c,f_rk);pip(x+sm+7,y+ch-sm-7,cd.s,sm)end
pip(x+I(cw/2),I(y+ch/2),cd.s,lg)
end
end
local function ai_pod(idx,px,py,pw,ph)
px,py,pw,ph=I(px),I(py),I(pw),I(ph);local p=seats[idx];local it=(state=="ai"and trn_s==idx)
box(px,py,pw,ph,p.fold and K_PFD or K_POD,it and K_GLD or(p.st=="WIN!"and C.good or 0x22303C),s2 and 8 or 5)
local hd=p.role~=""and(p.name.."·"..p.role)or p.name
local nc,sc=p.fold and K_DIM or(it and K_GLD or C.text),p.fold and K_DIM or K_GLD
local st,stc=p.st~=""and p.st or(p.all and"ALL-IN"or"WAIT"),p.fold and K_DIM or(p.st=="WIN!"and C.good or(it and C.accent or C.sub))
if s2 and ph>=110 then
cv:text(px+10,py+8,hd,nc,f_md);cv:text(px+10,py+10+lh_md,fmt("$%d",I(p.stack)),sc,f_md);cv:text(px+10,py+ph-lh_sm-8,st,stc,f_sm)
if#p.cards==2 and not p.fold then cbox(px+pw-96,py+12,42,58,p.cards[1],not show_all,3,6);cbox(px+pw-48,py+12,42,58,p.cards[2],not show_all,3,6)end
elseif s2 then
cv:text(px+12,py+6,hd,nc,f_md);cv:text(px+12,py+8+lh_md,fmt("$%d",I(p.stack)),sc,f_md);cv:text(px+12,py+10+lh_md*2,st,stc,f_sm)
if#p.cards==2 and not p.fold then local mh=ph-14;local mw=I(mh*0.72);cbox(px+pw-mw*2-14,py+7,mw,mh,p.cards[1],not show_all,3,6);cbox(px+pw-mw-8,py+7,mw,mh,p.cards[2],not show_all,3,6)end
else
cv:text(px+4,py+2,hd,nc,f_xs);cv:text(px+4,py+14,fmt("$%d",I(p.stack)),sc,f_sm);cv:text(px+4,py+27,st,stc,f_xs)
if#p.cards==2 and not p.fold and pw>=84 then
local w,h,o=show_all and 18 or 15,show_all and 25 or 22,show_all and 39 or 33
cbox(px+pw-o,py+4,w,h,p.cards[1],not show_all);cbox(px+pw-20,py+4,w,h,p.cards[2],not show_all)
end
end
end
local function brow(sx,y,r)
for i=1,5 do
local cx=sx+(i-1)*(cc_w+cc_g)
if board[i]then cbox(cx,y,cc_w,cc_h,board[i],false)else box(cx,y,cc_w,cc_h,K_FDK,K_FLN,r)end
end
end
local function fitx(s,w,z)
if tw(s,z)<=w then return s end
local t=s:gsub(" %(.-%)$","");if tw(t,z)<=w then return t end
return(t:gsub(" %$%d+",""))
end
local function opp_row(idx,x,y,w,h)
local p=seats[idx];local it=(state=="ai"and trn_s==idx)
box(x,y,w,h,p.fold and K_PFD or K_POD,it and K_GLD or(p.st=="WIN!"and C.good or 0x22303C),s2 and 8 or 5)
local hd=p.role~=""and(p.name.."·"..p.role)or p.name
local nc,sc=p.fold and K_DIM or(it and K_GLD or C.text),p.fold and K_DIM or K_GLD
local st,stc=p.st~=""and p.st or(p.all and"ALL-IN"or"WAIT"),p.fold and K_DIM or(p.st=="WIN!"and C.good or(it and C.accent or C.sub))
local pin,ms=s2 and 12 or 5,fmt("$%d",I(p.stack))
local l1=y+I((h-(lh_xs*2-2))/2)
cv:text(x+pin,l1,hd,nc,f_xs);cv:text(x+pin,l1+lh_xs-2,ms,sc,f_xs)
local rx=x+w-pin
if#p.cards==2 and not p.fold then
local mw,mh=PL.mcw,PL.mch;local my=y+I((h-mh)/2)
cbox(rx-mw*2-2,my,mw,mh,p.cards[1],not show_all);cbox(rx-mw,my,mw,mh,p.cards[2],not show_all)
rx=rx-mw*2-(s2 and 14 or 6)
end
local lx=x+pin+max(tw(hd,f_xs),tw(ms,f_xs))+(s2 and 12 or 6)
st=fitx(st,rx-lx,f_sm);if tw(st,f_sm)<=rx-lx then cv:text(rx-tw(st,f_sm),y+I((h-lh_sm)/2),st,stc,f_sm)end
end
local function title_p()
local sb,bb=blinds();local cx=I(W/2)
local px,py=s2 and 20 or 8,s2 and 16 or 6;local bw,bh,by=W-px*2,tbl_h-py*2,top_h+py
box(px,by,bw,bh,K_FDK,K_FLN,s2 and 12 or 8)
local sg=s2 and 18 or 8;local dw=I(clp(W*0.17,36,s2 and 110 or 56));local dh=I(dw*1.38)
local pl=fmt("$%d/$%d  ·  BUY-IN $%d",sb,bb,I(seats[1].stack))
local ph=lh_md+(s2 and 16 or 8);local plw=min(bw-8,tw(pl,f_md)+(s2 and 36 or 20))
local sa,sz=SUB_T:match("^(.-)%s+·%s+(.+)$")
local blk=lh_xl+4+lh_sm*(sz and 2 or 1)+sg+dh+sg+ph+sg+lh_sm
local y=by+max(sg,I((bh-blk)/2))
tcx(cx,y,TIT_T,K_GLD,f_xl);y=y+lh_xl+4
tcx(cx,y,sa or SUB_T,C.sub,f_sm);y=y+lh_sm
if sz then tcx(cx,y,sz,C.sub,f_sm);y=y+lh_sm end
y=y+sg
cbox(cx-dw-I(sg/2),y,dw,dh,{r=1,s=1},false,s2 and 4 or 3,s2 and 9 or 5);cbox(cx+I(sg/2),y,dw,dh,{r=1,s=2},false,s2 and 4 or 3,s2 and 9 or 5)
y=y+dh+sg
box(cx-I(plw/2),y,plw,ph,K_BAC,C.accent,5);tcx(cx,y+I((ph-lh_md)/2),pl,C.text,f_md);y=y+ph+sg
if y+lh_sm<=by+bh-4 then tcx(cx,y,fmt("WON %d OF %d HANDS",I(h_won),I(h_play)),C.sub,f_sm)end
end
local function table_p()
local hp,vg=PL.hp,PL.vg;local y=top_h+PL.y0+vg
for i=0,2 do opp_row(i+2,hp,y+i*(PL.oph+PL.og),W-hp*2,PL.oph)end
y=y+PL.oph*3+PL.og*2+vg
brow(I((W-(cc_w*5+cc_g*4))/2),y,s2 and 8 or 3);y=y+cc_h+vg
local p=seats[1]
if#p.cards==2 then cbox(hp,y,p_cw,p_ch,p.cards[1],false);cbox(hp+p_cw+PL.hg,y,p_cw,p_ch,p.cards[2],false)end
local ix=hp+p_cw*2+PL.hg+(s2 and 14 or 5);local iw,ih=W-hp-ix,p_ch
if state=="done"and b_tit~=""then
box(ix,y,iw,ih,0x0D131A,b_col,6)
local tz=(tw(b_tit,f_lg)<=iw-8)and f_lg or f_sm;local tt,sub=fitx(b_tit,iw-8,tz),fitx(b_sub,iw-8,f_sm)
local t1=th(tz);local ty1=y+I((ih-(t1+lh_sm))/2)
tcx(ix+I(iw/2),ty1,tt,b_col,tz);tcx(ix+I(iw/2),ty1+t1,sub,C.text,f_sm)
return
end
box(ix,y,iw,ih,K_POD,(state=="player")and C.accent or 0x22303C,5)
local pin=s2 and 12 or 5
local _,_,hn=eval_hand(p.cards,board);local hs=p.fold and"FOLDED"or hn
local ms=fmt("$%d",I(p.stack));local st=p.st~=""and p.st or(state=="player"and""or"WAITING")
local nl=(ih>=lh_sm*3+(s2 and 24 or 8))and 3 or 2
local stp=I((ih-nl*lh_sm)/(nl+1));local ly=y+stp
if nl==3 then cv:text(ix+pin,ly,p.role~=""and fmt("YOU [%s]",p.role)or"YOU",C.text,f_sm);ly=ly+lh_sm+stp end
cv:text(ix+pin,ly,hs,p.fold and C.bad or K_GLD,f_sm);ly=ly+lh_sm+stp
cv:text(ix+pin,ly,ms,C.text,f_sm)
local sx=ix+pin+tw(ms,f_sm)+(s2 and 12 or 6);st=fitx(st,ix+iw-pin-sx,f_sm)
if st~=""and tw(st,f_sm)<=ix+iw-pin-sx then cv:text(ix+iw-pin-tw(st,f_sm),ly,st,C.accent,f_sm)end
end
draw_screen=function()
local sb,bb=blinds();local cx=I(W/2)
cv:fill(K_FLT);local m=s2 and 8 or 3
cv:rect(m,top_h+m-1,W-m*2,tbl_h-(m-1)*2,K_FLN,false,s2 and 12 or 7)
cv:rect(0,0,W,top_h,K_BAR,true);cv:line(0,top_h,W,top_h,0x1F2933,1)
local tf=s2 and f_md or f_sm;local ty=max(1,I((top_h-th(tf))/2))
if port then
local ls,rs=fmt("POT $%d",I(pot)),(SNAMES[street]or"HOLD'EM")..(snd and""or" [M]")
if state=="title"then ls,rs=fmt("STACK $%d",I(seats[1].stack)),fmt("PEAK $%d%s",I(best_b),snd and""or" [M]")end
cv:text(pad,ty,ls,K_GLD,tf);cv:text(W-pad-tw(rs,tf),ty,rs,state=="title"and C.sub or C.accent,tf)
if state=="title"then title_p()else table_p()end
else
cv:text(pad,ty,fmt("STACK $%d",I(seats[1].stack)),K_GLD,tf)
local ms=(state=="title")and fmt("BLINDS $%d/$%d",sb,bb)or fmt("%s · $%d/$%d",SNAMES[street]or"HOLD'EM",sb,bb)
local mx=I((W-tw(ms,tf))/2);cv:text(mx,ty,ms,C.accent,tf)
local ps=fmt("PEAK $%d%s",I(best_b),snd and""or" [M]")
cv:text(max(mx+tw(ms,tf)+6,W-tw(ps,tf)-pad),ty,ps,C.sub,tf)
if state=="title"then
local px,py=s2 and 20 or 12,s2 and 16 or 8;local bw,bh,by=W-px*2,tbl_h-py*2,top_h+py
box(px,by,bw,bh,K_FDK,K_FLN,s2 and 12 or 8)
local pl=fmt("BLINDS: $%d / $%d   BUY-IN: $%d",sb,bb,I(seats[1].stack))
local dw,dh=s2 and 64 or 32,s2 and 90 or 44
if W>=280 then cbox(px+10,by+10,dw,dh,{r=1,s=1},false,s2 and 4 or 3,s2 and 9 or 5);cbox(W-px-dw-10,by+10,dw,dh,{r=1,s=2},false,s2 and 4 or 3,s2 and 9 or 5)end
tcx(cx,by+10,TIT_T,K_GLD,f_xl);local t2y=by+14+lh_xl
tcx(cx,t2y,SUB_T,C.sub,f_sm)
local pw,ph=tw(pl,f_md),lh_md+(s2 and 16 or 8);local plw=pw+(s2 and 36 or 20)
local p_y=max(t2y+lh_sm+8,I(by+(bh-ph)*0.54))
box(cx-I(plw/2),p_y,plw,ph,K_BAC,C.accent,5);tcx(cx,p_y+I((ph-lh_md)/2),pl,C.text,f_md)
local hy=by+bh-lh_sm-(s2 and 12 or 6)
if hy>p_y+ph+4 then tcx(cx,hy,W>=300 and"Tap [DEAL HOLD'EM] or press [D] to play"or"Tap [DEAL] below to play",C.sub,f_sm)end
else
local vp,hp,pg=s2 and 12 or 4,s2 and 16 or 6,s2 and 12 or 4
local py,ph=top_h+vp,s2 and I(tbl_h*0.27)or 40;local pw=I((W-hp*2-pg*2)/3)
for i=0,2 do ai_pod(i+2,hp+i*(pw+pg),py,pw,ph)end
local my=py+ph+vp;local ccx=W-hp-(cc_w*5+cc_g*4)
local pot_w=max(s2 and 220 or 92,ccx-hp-(s2 and 16 or 6))
box(hp,my,pot_w,cc_h,K_FDK,K_FLN,s2 and 8 or 5);local ptx=hp+(s2 and 14 or 5)
cv:text(ptx,my+(s2 and 8 or 4),"TOTAL POT",C.sub,f_xs);cv:text(ptx,my+(s2 and(10+lh_xs)or 16),fmt("$%d",I(pot)),K_GLD,f_lg)
local tc=I(max(0,c_bet-seats[1].sb))
cv:text(ptx,my+cc_h-lh_xs-(s2 and 8 or 3),(state=="done")and"HAND OVER"or(tc>0 and fmt("TO CALL: $%d",tc)or"CHECK OK"),C.accent,f_xs)
brow(ccx,my,s2 and 8 or 3)
local hy,p=my+cc_h+vp,seats[1];local hcw=s2 and p_cw or 34;local hcs=s2 and(hcw+8)or 36
if#p.cards==2 then cbox(hp,hy,hcw,p_ch,p.cards[1],false);cbox(hp+hcs,hy,hcw,p_ch,p.cards[2],false)end
local ix=hp+hcs+hcw+(s2 and 16 or 6);local iw,ih,iy=W-ix-hp,p_ch-(s2 and 0 or 4),hy+(s2 and 0 or 2)
if state=="done"and b_tit~=""then
box(ix,iy,iw,ih,0x0D131A,b_col,6);local ty1=iy+I((ih-(lh_lg+lh_sm))/2)
tcx(ix+I(iw/2),ty1,b_tit,b_col,f_lg);tcx(ix+I(iw/2),ty1+lh_lg+2,b_sub,C.text,f_sm)
else
box(ix,iy,iw,ih,K_POD,(state=="player")and C.accent or 0x22303C,5);local pin=s2 and 14 or 6
cv:text(ix+pin,iy+(s2 and 8 or 3),p.role~=""and fmt("YOU [%s]  BET $%d",p.role,I(p.sb))or fmt("YOU  BET $%d",I(p.sb)),C.text,f_sm)
local _,_,hn=eval_hand(p.cards,board);local hs=p.fold and"FOLDED"or hn
cv:text(ix+iw-tw(hs,f_sm)-pin,iy+(s2 and 8 or 3),hs,p.fold and C.bad or K_GLD,f_sm)
cv:text(ix+pin,iy+I((ih-lh_xs)/2)+1,(state=="player")and fmt("RAISE TARGET: $%d  [Up/Down]",I(r_to))or(p.st~=""and("STATUS: "..p.st)or"WAITING FOR TABLE..."),C.accent,f_xs)
cv:text(ix+pin,iy+ih-lh_xs-(s2 and 6 or 1),"[F]Fold  [C]Call/Chk  [R]Raise  [A]All-In",C.sub,f_xs)
end
end
end
local by=H-bot_h;cv:rect(0,by,W,bot_h,K_BAR,true);cv:line(0,by,W,by,0x1F2933,1)
local nb=#btns
if nb>0 then
local nr=1;for i=1,nb do nr=max(nr,btns[i].row or 1)end
local pv,bf,br,mp=s2 and 8 or 3,s2 and f_md or f_sm,s2 and 8 or 4,s2 and 20 or 8
if port then pv=PL.pv end
local bh,blh=I((bot_h-pv*(nr+1))/nr),th(bf)
for r=1,nr do
local ix={};for i=1,nb do if(btns[i].row or 1)==r then ix[#ix+1]=i end end
local n=#ix
local g=(not s2 and n>=5)and 3 or gap
local avw=W-g*(n+1)
local wts,tws,tot,wi={},{},0,1
for k=1,n do
local b=btns[ix[k]];local t_w=tw(b.label,bf);local wt=max(b.w or 60,t_w+mp)
wts[k],tws[k],tot=wt,t_w,tot+wt;if wt>wts[wi]then wi=k end
end
local bws,uw={},0
for k=1,n do bws[k]=I((avw*wts[k])/tot);uw=uw+bws[k]end
if n>0 and uw<avw then bws[wi]=bws[wi]+(avw-uw)end
local cbx=g
for k=1,n do
local i=ix[k];local b,bw=btns[i],bws[k];local bx,bby=cbx,by+pv+(r-1)*(bh+pv);cbx=cbx+bw+g
b.x,b.y,b.bw,b.bh=bx,bby,bw,bh
local bg=not b.enabled and K_BDS or(b.primary and K_BAC or K_BTN)
local fg=b.enabled and((i==f_idx)and(b.primary and K_GLD or C.accent)or 0x2B3642)or nil
box(bx,bby,bw,bh,bg,fg,br);local lbl,t_w=b.label,tws[k]
if t_w>bw-6 and lbl:find(" %[")then lbl=lbl:gsub(" %[.*%]","");t_w=tw(lbl,bf)end
if port and t_w>bw-6 then lbl=fitx(lbl,bw-6,bf);t_w=tw(lbl,bf)end
cv:text(bx+max(2,I((bw-t_w)/2)),bby+max(1,I((bh-blh)/2)),lbl,not b.enabled and K_DIM or(b.primary and K_GLD or C.text),bf)
end
end
end
end
local function cyc_foc(d)
local n=#btns;if n==0 then return end
for _=1,n do f_idx=((f_idx-1+d)%n)+1;if btns[f_idx]and btns[f_idx].enabled then break end end
draw_screen()
end
function app.on_open(w,h)
local brd=(sys.board and sys.board())or{}
W=I((w and w>0)and w or((brd.w and brd.w>0)and brd.w or 320))
H=I((h and h>0)and h or((brd.h and brd.h>0)and brd.h or 208))
layout()
seats[1].stack=I(tonumber(store.get("he_bank",S0))or S0)
best_b=I(tonumber(store.get("he_best",S0))or S0)
h_won=I(tonumber(store.get("he_wins",0))or 0)
h_play=I(tonumber(store.get("he_hands",0))or 0)
b_idx=I(clp(tonumber(store.get("he_blind",1))or 1,1,#BLINDS))
local _,bb=blinds();if seats[1].stack<bb then seats[1].stack=S0 end
for i=2,4 do seats[i].stack=S0 end
cv=make_cv(W,H);build_deck();state="title";rebuild_btns();draw_screen()
end
function app.on_tick()
if state~="ai"then timer.stop();return end
local act,can=cnt_pl();if act<=1 then resolve(true);return end
if can<2 then next_street();return end
local p=seats[trn_s]
if p and not p.hum and not p.fold and not p.all then ai_go(trn_s)else adv_turn()end
end
function app.on_input(ev)
if not ev then return end
if ev.type=="down"then
local ex,ey=tonumber(ev.x)or 0,tonumber(ev.y)or 0
for i=1,#btns do
local b=btns[i]
if b.x and ex>=b.x and ex<=b.x+b.bw and ey>=b.y and ey<=b.y+b.bh then
if b.enabled and b.fn then f_idx=i;b.fn()end;return
end
end
if ey<=top_h and ex>=W-I(W*0.28)then snd=not snd;sys.toast(snd and"Sound ON"or"Sound OFF");draw_screen();return end
if state=="ai"then app.on_tick(0);return end
if(state=="done"or state=="title")and ey>top_h+2 and ey<H-bot_h-2 then start_hand();return end
elseif ev.type=="swipe"then
local _,bb=blinds();local d=ev.dir
if d=="left"then cyc_foc(-1)elseif d=="right"then cyc_foc(1)
elseif d=="up"and state=="player"then adj_r(bb)elseif d=="down"and state=="player"then adj_r(-bb)end
elseif ev.type=="key"then
local _,bb=blinds();local k=tostring(ev.key or""):lower()
if state=="ai"then app.on_tick(0);return end
if k=="left"then cyc_foc(-1)elseif k=="right"or k=="l"then cyc_foc(1)
elseif(k=="up"or k=="w"or k=="+"or k=="=")and state=="player"then adj_r(bb)
elseif(k=="down"or k=="z"or k=="-")and state=="player"then adj_r(-bb)
elseif k=="enter"or k==" "then local b=btns[f_idx];if b and b.enabled and b.fn then b.fn()end
elseif k=="m"then snd=not snd;sys.toast(snd and"Sound ON"or"Sound OFF");draw_screen()
elseif state=="player"then
if k=="f"then act_pl(1,"fold")elseif k=="c"then act_pl(1,"call")
elseif k=="r"and seats[1].stack>(c_bet-seats[1].sb)then act_pl(1,"raise",r_to)
elseif k=="a"then act_pl(1,"allin")end
elseif state=="done"or state=="title"then
if k=="d"or k=="h"then start_hand()elseif k=="b"then cyc_bl()elseif k=="n"then rst_tbl()end
end
end
end
function app.on_close()save_stats()end
return app
