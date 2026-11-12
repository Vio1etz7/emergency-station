# 开发环境与 Git 协作说明


---

## 零、全局流程图

```
        ┌──────────────┐
        │  git pull    │  先把别人改的东西拿下来
        └──────┬───────┘
               ↓
        ┌──────────────┐
        │  写代码/改配置 │  只改自己那个目录
        └──────┬───────┘
               ↓
        ┌──────────────┐
        │  编译 + 烧录   │  编译不过就别提交
        └──────┬───────┘
               ↓
        ┌──────────────┐
        │ git commit   │  写清楚改了什么
        └──────┬───────┘
               ↓
        ┌──────────────┐
        │  git push    │  推上去，别人才能拿到
        └──────────────┘
```

**口诀：先拉、再改、能编译、再推。**

---

## 一、需要准备的三样东西

| 东西 | 怎么来的 | 备注 |
|---|---|---|
| Ubuntu 虚拟机 | 小熊派官网的镜像 | 已装好，不用管 |
| git | `sudo apt install -y git` | 已装好可跳过 |
| GitHub 账号 + Token | 见第三节 | **每人一个**，不能共用 |

---

## 二、首次配置（每人只做一次）

打开 Ubuntu 终端（`Ctrl+Alt+T`），逐条执行，**把引号里的内容换成自己的**：

```bash
git config --global user.name  "名字"
git config --global user.email "1732115984@qq.com"
git config --global core.autocrlf input
git config --global pull.rebase false
```

四条命令的作用：

| 命令 | 作用 | 
|---|---|
| `user.name` | 提交记录里显示的名字 | 
| `user.email` | 关联到 GitHub 账号 |
| `core.autocrlf input` | 不自动改换行符 | 
| `pull.rebase false` | pull 时用合并而不是变基 | 

验证：

```bash
git config --global --list
```

能看到刚才那四行就成了。

---

## 三、生成 Personal Access Token（PAT）

**GitHub 从 2021 年起不再接受账号密码推送**，必须用 Token。这一步不做，`git push` 一定失败。

1. 浏览器打开 GitHub → 右上角头像 → **Settings**
2. 左侧拉到最下面 → **Developer settings**
3. **Personal access tokens** → **Tokens (classic)**
4. 右上 **Generate new token** → **Generate new token (classic)**
5. Note 随便写（比如 `bearpi`），Expiration 选 **90 days**
6. 勾选权限：**只要勾 `repo`**（它会把下面几个子项自动勾上）
7. 拉到底 → **Generate token**
8. **立刻复制** `ghp_xxxxxxxx` 这串，**关掉页面就再也看不到了**

存哪儿：记事本里存一份，或者干脆在 Ubuntu 里建个文件放着（别提交进仓库）。

**忘记/泄露了怎么办**：回到同一页面 → 删除旧的 → 重新生成一个。

---

## 四、把仓库拉下来（第一次）

### 前提

OpenHarmony 工程已经在 `~/project/bearpi-hm_nano`，而且**能编译通过**。

### 关键：仓库必须放在 sample 目录里

```bash
cd ~/project/bearpi-hm_nano/applications/BearPi/BearPi-HM_Nano/sample

git clone https://github.com/Vio1etz7/emergency-station
```

**为什么必须放这儿？** 因为 `BUILD.gn` 里写的是相对路径：

```gn
"emergency-station/E1_yizhan_node:yizhan_node",
```

换个位置放，编译时 GN 就找不到这个 app 了。

### 克隆时会问什么

```
Username for 'https://github.com': 你的GitHub用户名
Password for 'https://xxx@github.com': 粘贴Token（不是登录密码）
```

> 粘贴时**屏幕上不显示任何字符**，这是正常的，粘完直接回车。

成功后会看到 `Cloning into 'emergency-station'...`，然后 `ls` 能看到目录。

### 以后别人更新了，怎么拿到最新代码

```bash
cd ~/project/bearpi-hm_nano/applications/BearPi/BearPi-HM_nano/sample/emergency-station
git pull
```

---

## 五、日常开发五步（每天就这么干）

```bash
# 1. 进仓库
cd ~/project/bearpi-hm_nano/applications/BearPi/BearPi-HM_nano/sample/emergency-station

# 2. 拉最新代码
git pull

# 3. 改代码 
#    cd E1_yizhan_node / E3_ir_node / E4_box_node ...

# 4. 编译（在工程根目录）
cd ~/project/bearpi-hm_nano
python build.py BearPi-HM_Nano


# 5. 提交并推送
cd applications/BearPi/BearPi-HM_nano/sample/emergency-station
git status                    # 确认只改了自己的文件
git add .                     # 或者 git add 
git commit -m "feat(ir): 完成红外检测与上报"
git push
```

---

## 六、提交信息怎么写

格式：`类型(模块): 中文描述`

| 类型 | 什么时候用 | 例子 |
|---|---|---|
| `feat` | 加功能 | `feat(ir): 加 20ms 消抖` |
| `fix` | 修 bug | `fix(gw): 修端口冲突导致收不到包` |
| `docs` | 只改文档 | `docs: 补周报 11-19` |
| `chore` | 杂活 | `chore: 更新 .gitignore` |

模块名用节点代号：`gw`（主控）、`lamp`、`ir`、`box`、`repo`。


---

## 七、切换编译哪个节点

`sample/BUILD.gn` 里一次只能开一个，改完**不要提交**（这个文件属于 OpenHarmony 源码，不在我们仓库里）：

```gn
features = [
    "emergency-station/E1_yizhan_node:yizhan_node",   # 编主控时开这个
    #"emergency-station/E3_ir_node:ir_node",          # 编红外时换成这个
]
```

规则：**注释掉一个，打开一个**，只留一行没被注释的。

---

## 八、冲突怎么办

两个人改了同一个文件的同一处，`git pull` 时会报：

```
CONFLICT (content): Merge conflict in xxx
```

打开那个文件，会看到：

```
<<<<<<< HEAD
你自己原来的代码
=======
别人推上来的代码
>>>>>>> origin/master
```

处理：**手工决定留哪段**，把 `<<<<<<<`、`=======`、`>>>>>>>` 三行全删掉，然后：

```bash
git add 那个文件
git commit -m "merge: 解决冲突"
git push
```

**怎么避免**：只改自己目录，几乎不会冲突。唯一可能撞车的是 `README.md` 和 `docs/`——要改这些，先在群里说一声。

---


## 九、常见报错对照表

| 报错 | 原因 | 怎么办 |
|---|---|---|
| `Authentication failed` / `403` | Token 错了或过期 | 重新生成 Token，`git push` 时重输 |
| `Support for password authentication was removed` | 用了登录密码 | 换成 Token |
| `rejected - non-fast-forward` | 别人先推了，你没 pull | `git pull` 后再 `git push` |
| `fatal: not a git repository` | 当前目录不是我们的仓库 | `cd` 回 `emergency-station` 再操作 |
| `warning: LF will be replaced by CRLF` | `autocrlf` 没设成 `input` | 重跑第二条配置命令 |
| `could not read Username` | 非交互终端 | 直接在 Ubuntu 图形终端里操作 |
| 卡在 `Enumerating objects` | 网络 | 见第九节配代理 |
| `Permission denied (publickey)` | 用了 SSH 地址但没配密钥 | 改用 HTTPS 地址 |

---

## 十、绝对不要做的事

1. **不要在 `sample/` 目录里 `git add .`** ——那是小熊派官方仓库，几万个文件，推不上去还会把环境搞乱
2. **不要提交 `out/`、`*.bin`、`*.o`** ——几百 MB，GitHub 会拒收（`.gitignore` 已挡住，但别手动 `git add -f`）
3. **不要提交 `sample/BUILD.gn`** ——它不在我们仓库里，但如果误拷进来要删掉
4. **不要 `git push -f`** ——会覆盖别人的代码，出事了很难救
5. **不要在 Windows 侧对 `Z:` 盘跑 git** ——网络盘 + 换行符转换，容易把仓库搞坏。**所有 git 操作在 Ubuntu 终端里做**

---

## 十二、每天开工/收工清单

**开工**

```bash
cd ~/project/bearpi-hm_nano/applications/BearPi/BearPi-HM_nano/sample/emergency-station
git pull
```

**收工前**

```bash
git status          # 有没有忘了提交的
git add .
git commit -m "feat(xxx): 今天做了什么"
git push
```

**周日**（组长做）

```bash
git tag v0.1-week1
git push --tags
```

打 tag 是为了万一改崩了，能回到上周的状态。

---

## 附：常用命令速查

```bash
git status                  # 现在改了哪些文件
git diff                    # 具体改了什么内容
git log --oneline -10       # 最近 10 次提交
git pull                    # 拉最新
git add 某文件               # 只加这一个文件
git commit -m "说明"         # 提交
git push                    # 推上去
git checkout -- 某文件       # 撤销对某个文件的修改（没提交前）
git stash                   # 临时收起改动（切分支前用）
```
