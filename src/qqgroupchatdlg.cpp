#include "qqgroupchatdlg.h"
#include "ui_qqgroupchatdlg.h"

#include <QScrollBar>
#include <QDateTime>
#include <QHttpRequestHeader>
#include <QMouseEvent>
#include <QDebug>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QRegExp>

#include "include/json.h"

#include "qqitemmodel.h"
#include "core/qqutility.h"
#include "core/groupimgloader.h"
#include "core/groupimgsender.h"
#include "core/qqskinengine.h"
#include "core/captchainfo.h"
#include "core/groupchatlog.h"

QQGroupChatDlg::QQGroupChatDlg(QString gid, QString name, QString group_code, QString avatar_path, QWidget *parent) :
    QQChatDlg(gid, name, parent),
    ui(new Ui::QQGroupChatDlg()),
    group_code_(group_code),
    member_root_(new QQItem())
{
   ui->setupUi(this);
   updateSkin();
   te_input_.setFocus();

   set_type(QQChatDlg::kGroup);

   ui->btn_send_key->setMenu(send_type_menu_);

   ui->splitter_left_->insertWidget(0, &msgbrowse_);
   ui->v_layout_left_->insertWidget(1, &te_input_);

   ui->splitter_main->setChildrenCollapsible(false);
   ui->splitter_left_->setChildrenCollapsible(false);

   //设置分割器大小
   QList<int> right_sizes;
   right_sizes.append(500);
   right_sizes.append(ui->splitter_right_->midLineWidth());
   ui->splitter_main->setSizes(right_sizes);

   QList<int> left_sizes;
   left_sizes.append(500);
   left_sizes.append(ui->splitter_left_->midLineWidth());
   ui->splitter_left_->setSizes(left_sizes);

   this->resize(600, 500);

   //QScrollBar *bar = te_messages_.verticalScrollBar();
   //connect(bar, SIGNAL(rangeChanged(int, int)), this, SLOT(silderToBottom(int, int)));
   connect(ui->btn_send_img, SIGNAL(clicked(bool)), this, SLOT(openPathDialog(bool)));
   connect(ui->btn_send_msg, SIGNAL(clicked()), this, SLOT(sendMsg()));
   connect(ui->btn_qqface, SIGNAL(clicked()), this, SLOT(openQQFacePanel()));
   connect(ui->btn_close, SIGNAL(clicked()), this, SLOT(close()));
   connect(ui->btn_chat_log, SIGNAL(clicked()), this, SLOT(openChatLogWin()));

   ui->lbl_name_->setText(name_);
   setWindowTitle(name_);

   send_url_ = "/channel/send_qun_msg2";

   if (avatar_path.isEmpty())
       avatar_path = QQSkinEngine::instance()->getSkinRes("default_group_avatar");

   QFile file(avatar_path);
   file.open(QIODevice::ReadOnly);
   QPixmap pix;
   pix.loadFromData(file.readAll());
   file.close();

   ui->lbl_avatar_->setPixmap(pix);

   getGfaceSig();
}

QQGroupChatDlg::~QQGroupChatDlg()
{
    disconnect(this);
    delete ui;
}

void QQGroupChatDlg::updateSkin()
{

}

void QQGroupChatDlg::closeEvent(QCloseEvent *)
{
    writeMemberInfoToSql();
    emit chatFinish(this);
}

ImgLoader *QQGroupChatDlg::getImgLoader() const
{
    return new GroupImgLoader();
}

QQChatLog *QQGroupChatDlg::getChatlog() const
{
    return new GroupChatLog(group_code_);
}

ImgSender* QQGroupChatDlg::getImgSender() const
{
    return new GroupImgSender();
}

void QQGroupChatDlg::getGfaceSig()
{
    bool need_create_table = true;
    {
        QStringList connection_names = QSqlDatabase::connectionNames();
        QSqlDatabase db;
        if (connection_names.isEmpty())
        {
            db = QSqlDatabase::addDatabase("QSQLITE");
            db.setDatabaseName("qqgroupdb");
        }
        else
        {
            db = QSqlDatabase::database(connection_names[0]);
        }

        if (!db.open())
            return;

        connection_name_ = db.connectionName();
        QSqlQuery query;
        query.exec("SELECT count(*) FROM sqlite_master WHERE type='table' and name='groupmemberinfo'");

        if (!query.first())
            qDebug()<<query.lastError()<<endl;

        bool exist_group_sig_table = query.value(0).toBool();

        if (exist_group_sig_table)
        {
            QSqlQuery query;
            QString command = "SELECT count(*) FROM groupsig WHERE gid == %1";
            query.exec(command.arg(id_));

            if (!query.first())
                qDebug()<<query.lastError()<<endl;

            int exist_record_count = query.value(0).toInt();

            if (exist_record_count != 0)
            {
                need_create_table = false;
                readSigFromSql();
            }
        }
    }
    if (need_create_table)
    {
        QString gface_sig_url = "/channel/get_gface_sig2?clientid=5412354841&psessionid="+CaptchaInfo::singleton()->psessionid() +
                "&t="+QString::number(QDateTime::currentMSecsSinceEpoch());

        QHttpRequestHeader header;
        header.setRequest("GET", gface_sig_url);
        header.addValue("Host", "d.web2.qq.com");
        header.addValue("Referer", "http://d.web2.qq.com/proxy.html?v=20110331002");
        header.addValue("Cookie", CaptchaInfo::singleton()->cookie());

        http_.setHost("d.web2.qq.com");
        connect(&http_, SIGNAL(done(bool)), this, SLOT(getGfaceSigDone(bool)));

        http_.request(header);
    }
    else
        getGroupMemberList();
}

void QQGroupChatDlg::readSigFromSql()
{
    QSqlQuery query;
    QString read_command = "SELECT * FROM groupsig WHERE gid == %1";
    query.exec(read_command.arg(id_));

    while (query.next())
    {
        QString key = query.value(1).toString();
        QString sig = query.value(2).toString();

        gface_key_ = key;
        gface_sig_ = sig;
    }
}

void QQGroupChatDlg::getGfaceSigDone(bool err)
{
    Q_UNUSED(err)
    disconnect(&http_, SIGNAL(done(bool)), this, SLOT(getGfaceSigDone(bool)));

    QByteArray array = http_.readAll();

    int gface_key_idx = array.indexOf("gface_key")+12;
    int gface_key_end_idx = array.indexOf(",",gface_key_idx)-1;

    int gface_sig_idx = array.indexOf("gface_sig")+12;
    int gface_sig_end_idx = array.indexOf("}", gface_sig_idx)-1;

    gface_key_ = array.mid(gface_key_idx, gface_key_end_idx - gface_key_idx);
    gface_sig_ = array.mid(gface_sig_idx, gface_sig_end_idx - gface_sig_idx);

    {
    QSqlDatabase db = QSqlDatabase::database(connection_name_);

    if (!db.open())
        return;

    QSqlQuery query;
    QSqlDatabase::database().transaction();

    createSigSql();

    QString insert_command = "INSERT INTO groupsig VALUES (%1, '%2', '%3')";
    query.exec(insert_command.arg(id_).arg(gface_key_).arg(gface_sig_));

    QSqlDatabase::database().commit();
    }

   getGroupMemberList();
}

void QQGroupChatDlg::createSigSql()
{
    QSqlQuery query;

    query.exec("CREATE TABLE IF NOT EXISTS groupsig ("
        "gid INTERGER,"
        "key VARCHAR(15),"
        "sig VARCHAR(50),"
        "PRIMARY KEY (gid))");

    if (query.lastError().isValid())
    {
        qDebug()<<query.lastError();
    }
}

void QQGroupChatDlg::parseGroupMemberList(const QByteArray &array, QQItem *const root_item)
{
    Json::Value root;
    Json::Reader reader;

    if (!reader.parse(QString(array).toStdString(), root, false))
        return;

    Json::Value members = root["result"]["minfo"];

    for (unsigned int i = 0; i < members.size(); ++i)
    {
        QString nick = QString::fromStdString(members[i]["nick"].asString());
        QString uin = QString::number(members[i]["uin"].asLargestInt());

        QQItem *info = new QQItem(QQItem::kFriend, nick, uin, root_item);
        info->set_status(kOffline);
        root_item->children_.append(info);

        convertor_.addUinNameMap(uin, nick);
    }

    Json::Value stats = root["result"]["stats"];

    for (unsigned int i = 0; i < stats.size(); ++i)
    {
        ClientType client_type = (ClientType)stats[i]["client_type"].asInt();

        FriendStatus stat = (FriendStatus)stats[i]["stat"].asInt();
        QString uin = QString::number(stats[i]["uin"].asLargestInt());

        QQItem *item  = NULL;
        int j;
        for (j = 0; j < root_item->children_.count(); ++j)
        {
            if (root_item->children_[j]->id() == uin)
            {
                item = root_item->children_[j];
                break;
            }
        }

        item->set_status(stat);
        item->set_clientType(client_type);

        root_item->children_.remove(j);
        root_item->children_.push_front(item);
    }
}

void QQGroupChatDlg::createSql()
{
    QSqlQuery query;

    query.exec("CREATE TABLE IF NOT EXISTS groupmemberinfo ("
        "uin INTEGER,"
        "gid INTERGER,"
        "name VARCHAR(15),"
        "status INTEGER,"
        "avatarpath VARCHAR(20),"
        "PRIMARY KEY (uin))");

    if (query.lastError().isValid())
    {
        qDebug()<<query.lastError();
    }
}

void QQGroupChatDlg::readFromSql()
{
    QSqlQuery query;
    QString read_command = "SELECT * FROM groupmemberinfo WHERE groupmemberinfo.gid == %1";
    query.exec(read_command.arg(id_));
    
    QQItemModel *model = new QQItemModel(this);
    model->setIconSize(QSize(25, 25));

    while (query.next())
    {
        QString uin = query.value(0).toString();
        QString nick = query.value(2).toString();
        FriendStatus stat = (FriendStatus)query.value(3).toInt();
        QString avatar_path = query.value(4).toString();

        QQItem *info = new QQItem(QQItem::kFriend, nick, uin, member_root_);
        info->set_status(stat);
        info->set_avatarPath(avatar_path);

        if (info->status() == kOffline)
            member_root_->children_.push_back(info);
        else
            member_root_->children_.push_front(info);

        convertor_.addUinNameMap(uin, nick);
    }

    model->setRoot(member_root_);
    ui->lv_members_->setModel(model);

    QString drop_command = "DROP TABLE IF EXISTS groupmemberinfo";
    query.exec(drop_command);

    replaceUnconverId();
}

void QQGroupChatDlg::replaceUnconverId()
{
    foreach (QString id, unconvert_ids_)
    {
        msgbrowse_.replaceIdToName(id, convertor_.convert(id));
    }
}

void QQGroupChatDlg::writeMemberInfoToSql()
{
    {
        QStringList connection_names = QSqlDatabase::connectionNames();
        QSqlDatabase db;
        if (connection_names.isEmpty())
        {
            db = QSqlDatabase::addDatabase("QSQLITE");
            db.setDatabaseName("qqgroupdb");
        }
        else
        {
            db = QSqlDatabase::database(connection_names[0]);
        }

        if (!db.open())
            return;

        QSqlQuery query;
        QSqlDatabase::database().transaction();

        createSql();
        for (int i = 0; i < member_root_->children_.count(); ++i)
        {
            QQItem *item = member_root_->children_[i];

            QString insert_command = "INSERT INTO groupmemberinfo VALUES (%1, %2, '%3', %4, '%5')";
            query.exec(insert_command.arg(item->id()).arg(id_).arg(item->name()).arg(item->status()).arg(item->avatarPath()));
        }
        QSqlDatabase::database().commit();
    }
    QString name;{
        name = QSqlDatabase::database().connectionName();
        QSqlDatabase::database().close();
    }
    QSqlDatabase::removeDatabase(name);
}

QQItem *QQGroupChatDlg::findItemById(QString id) const
{
    QQItem *item  = NULL;
    foreach (item, member_root_->children_)
    {
        if (item->id() == id)
            return item;
    }
    return NULL;
}

void QQGroupChatDlg::getInfoById(QString id, QString &name, QString &avatar_path, bool &ok) const
{
    foreach (QQItem *item, member_root_->children_)
    {
        if (item->id() == id)
        {
            name = item->name();
            avatar_path = item->avatarPath().isEmpty() ? QQSkinEngine::instance()->getSkinRes("default_friend_avatar") : item->avatarPath();
            ok = true;
            return;
        }
    }
    name = convertor_.convert(id);
    avatar_path = QQSkinEngine::instance()->getSkinRes("default_friend_avatar");
    ok = false;
}

void QQGroupChatDlg::getGroupMemberList()
{
    {
        QSqlDatabase db = QSqlDatabase::database(connection_name_);

        if (!db.open())
            return;

        QSqlQuery query;
        query.exec("SELECT count(*) FROM sqlite_master WHERE type='table' and name='groupmemberinfo'");

        if (!query.first())
            qDebug()<<query.lastError()<<endl;

        bool exist_group_member_table = query.value(0).toBool();

        bool need_create_table = true;
        if (exist_group_member_table)
        {
            query.exec("SELECT count(*) FROM groupmemberinfo WHERE gid == " + id_);

            if (!query.first())
                qDebug()<<query.lastError()<<endl;

            int exist_record_count = query.value(0).toInt();

            if (exist_record_count != 0)
            {
                need_create_table = false;
                readFromSql();
            }
        }
        if (need_create_table)
        {
            QString get_group_member_url = "/api/get_group_info_ext2?gcode=" + group_code_ + "&vfwebqq=" +
                    CaptchaInfo::singleton()->vfwebqq() + "&t="+ QString::number(QDateTime::currentMSecsSinceEpoch());

            QHttpRequestHeader header("GET", get_group_member_url);
            header.addValue("Host", "s.web2.qq.com");
            header.addValue("Referer", "http://s.web2.qq.com/proxy.html?v=20110412001");
            header.addValue("Cookie", CaptchaInfo::singleton()->cookie());

            http_.setHost("s.web2.qq.com");
            connect(&http_, SIGNAL(done(bool)), this, SLOT(getGroupMemberListDone(bool)));

            http_.request(header);
        }
    }

    QString name;{
        name = QSqlDatabase::database().connectionName();
        QSqlDatabase::database().close();
    }
   QSqlDatabase::removeDatabase(name);
}

void QQGroupChatDlg::getGroupMemberListDone(bool err)
{
    Q_UNUSED(err)
    disconnect(&http_, SIGNAL(done(bool)), this, SLOT(getGroupMemberListDone(bool)));

    QByteArray groups_member_info = http_.readAll();
    http_.close();

    QQItemModel *model = new QQItemModel(this);
    model->setIconSize(QSize(25, 25));
    parseGroupMemberList(groups_member_info, member_root_);
    model->setRoot(member_root_);

    ui->lv_members_->setModel(model);

   replaceUnconverId();
}

QString QQGroupChatDlg::converToJson(const QString &raw_msg)
{
    bool has_gface = false;
    QString msg_template;

    //提取<p>....</p>内容
    QRegExp p_reg("(<p.*</p>)");
    p_reg.setMinimal(true);

    int pos = 0;
    while ( (pos = p_reg.indexIn(raw_msg, pos)) != -1 )
    {
        QString content = p_reg.cap(0);
        while (!content.isEmpty())
        {
            if (content[0] == '<')
            {           
                int match_end_idx = content.indexOf('>')+1;
                QString single_chat_item = content.mid(0, match_end_idx);

                int img_idx = single_chat_item.indexOf("src");
                if (img_idx != -1)
                {
                    img_idx += 5;
                    int img_end_idx = content.indexOf("\"", img_idx);
                    QString src = content.mid(img_idx, img_end_idx - img_idx);

                    if (src.contains(kQQFacePre))
                    {
                        msg_template.append("[\\\"face\\\"," + src.mid(kQQFacePre.length()) + "],");
                    }
                    else
                    {
                        has_gface = true;
                        msg_template.append("[\\\"cface\\\",\\\"group\\\",\\\"" + id_file_hash_[src].name + "\\\"],");
                    }
                    //                if (src.contains("-"))
                    //                {
                    //                    has_gface = true;
                    //                    msg_template.append("[\\\"cface\\\",\\\"group\\\",\\\"" + id_file_hash_[src].name + "\\\"],");
                    //                }
                    //                else
                    //                {
                    //                    msg_template.append("[\\\"face\\\"," + src + "],");
                    //                }
                }

                content = content.mid(match_end_idx);
            }
            else
            {
                int idx = content.indexOf("<");
                msg_template.append("\\\"" + content.mid(0, idx).replace("&amp;", "%26").replace('+', "%2B").replace(';', "%3B") + "\\\",");
                if (idx == -1)
                    content = "";
                else
                    content = content.mid(idx);
            }
        }

        msg_template.append("\\\"\\\\n\\\",");
        pos += p_reg.cap(0).length();
    }

    msg_template = msg_template +
            "[\\\"font\\\",{\\\"name\\\":\\\"%E5%AE%8B%E4%BD%93\\\",\\\"size\\\":\\\"10\\\",\\\"style\\\":[0,0,0],\\\"color\\\":\\\"000000\\\"}]]\","
            "\"msg_id\":" + QString::number(msg_id_++) + ",\"clientid\":\"5412354841\","
            "\"psessionid\":\""+ CaptchaInfo::singleton()->psessionid() +"\"}"
            "&clientid=5412354841&psessionid="+CaptchaInfo::singleton()->psessionid();

    if (has_gface)
        msg_template = "r={\"group_uin\":" + id_ +",\"group_code\":" + group_code_ + "," + "\"key\":\"" + gface_key_ + "\"," +
            "\"sig\":\"" + gface_sig_ + "\", \"content\":\"[" + msg_template;
    else
        msg_template = "r={\"group_uin\":" + id_ + ",\"content\":\"[" + msg_template;

    return msg_template;
}