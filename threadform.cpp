#include "threadform.h"
#include "ui_threadform.h"
#include "threadformcontext.h"
#include "filter.h"
#include "threadtab.h"
#include "mainwindow.h"
#include <QPixmap>
#include <QImageReader>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDesktopServices>
#include <QSettings>
#include <QStringList>
#include <QListIterator>
#include <QPainter>
#include <QMenu>


//TODO get rid of #include threadtab.h and mainwindow.h by using signals/slots
//TODO Possibly decouple the file and thumb getters to another class class
ThreadForm::ThreadForm(Chan *api, ThreadFormStrings strings, bool root, bool autoExpand, QWidget *parent, int replyLevel) :
	QWidget(parent), api(api), strings(strings), root(root), autoExpand(autoExpand), replyLevel(replyLevel), tab(parent),
	ui(new Ui::ThreadForm)
{
	if(root) rootTF = this;
	ui->setupUi(this);
	this->board = strings.board;
	if(strings.thread == "index") this->type = PostType::Thread;
	else this->type = PostType::Reply;
	for(int i=0;i<replyLevel;i++){
		if(i == replyLevel-1){
			background.setRgb(darkness,darkness,darkness);
			ui->hide->setStyleSheet("padding: 0 10px; background-color:"+ background.name());
		}
		darkness = darkness*0.8;
	}
	background.setRgb(darkness,darkness,darkness);
	this->setStyleSheet("background-color:" + background.name() + "; color:#bbbbbb;");
	ui->quoteWidget->hide();
	ui->tim->hide();
	ui->fileInfo->hide();
	QSettings settings(QSettings::IniFormat,QSettings::UserScope,"qtchan","qtchan");
	setFontSize(settings.value("fontSize",14).toInt());
	connect(ui->hide,&ClickableLabel::clicked,this,&ThreadForm::hideClicked);
	comQuoteConnection = connect(ui->com,&QLabel::linkActivated,this,&ThreadForm::quoteClicked);
	infoQuoteConnection = connect(ui->info,&QLabel::linkActivated,this,&ThreadForm::quoteClicked);
	connect(this,&ThreadForm::setPixmap,this,&ThreadForm::onSetPixmap,Qt::UniqueConnection);
	if(root) ui->hide->setStyleSheet("padding:0 10px; background-color: #191919;");
	ui->info->installEventFilter(this);
	ui->com->installEventFilter(this);
	ui->fileInfo->installEventFilter(this);
	this->installEventFilter(this);
}

void ThreadForm::appendQuote()
{
	static_cast<ThreadTab*>(tab)->quoteIt(">>"+post.no);
}

ThreadForm::~ThreadForm()
{
	inserted.clear();
	foreach(QNetworkReply *reply, networkReplies){
		reply->abort();
		reply->deleteLater();
	}
	//disconnect clones quote clicking in notifications
	foreach(QPointer<ThreadForm> tf, clones){
		if(tf){
			tf->disconnect(tf->comQuoteConnection);
			tf->disconnect(tf->infoQuoteConnection);
		}
	}
	delete ui;
}

void ThreadForm::setText(QString text)
{
	ui->com->setText(text);
}

QString ThreadForm::infoString(){
	tfInfoString = "<span style=\"color: rgb(152, 125, 62); font-weight: bold;\">" % post.sub % "</span> " +
			"<span style=\"color: rgb(163, 68, 67);\">" % post.name % "</span> " %
			strings.countryString % regionString %
			"<span>" % post.realNow % "</span> " %
			"<a href=\"#op" % post.no % "\" style=\"color:#bbbbbb; text-decoration: none\">No." % post.no % "</a> " %
			"<a href=\"#f" % post.no % "\" style=\"color:#897399; text-decoration: none\"></a> " %
			repliesString;
	return tfInfoString;
}

QString ThreadForm::matchThis(){
	return post.sub % post.com % tfInfoString % fileInfoString;
}

void ThreadForm::load(Post &post)
{
	//set post number
	this->post = post;
	//post = new Post(p,board);
	ui->info->setText(infoString());
	ui->com->setText(post.com);

	getFlag();

	//set image
	//TODO use filedeleted image
	if(!post.tim.isNull() && !post.filedeleted) {
		file = new QFile(strings.filePath,this);
		ui->fileInfo->show();
		ui->fileInfo->setText(strings.fileInfoString);
		thumb = new QFile(strings.thumbPath,this);
		getThumb();
		if(autoExpand || file->exists()) getFile();
		connect(ui->tim,&ClickableLabel::clicked,this,&ThreadForm::imageClicked);
	}
	else{
		ui->pictureLayout->deleteLater();
		this->hasImage = false;
	}
}

void ThreadForm::getFlag(){
	if(post.country_name.isEmpty()) return;
	setInfoString();
	downloadFile(strings.flagUrl,strings.flagPath,nc.thumbManager);
}

void ThreadForm::getFile(bool andOpen){
	if(post.tim.isNull() || post.filedeleted || gettingFile) return;
	QString url = api->apiBase() + strings.fileUrl;
	QString message = andOpen ? "clicked" : "";
	downloadFile(url,strings.filePath,nc.fileManager,message);
	gettingFile = true;
}

void ThreadForm::getThumb(){
	if(post.tim.isNull() || post.filedeleted) return;
	QString url = api->apiBase() + strings.thumbUrl;
	downloadFile(url,strings.thumbPath,nc.thumbManager);
	gettingThumb = true;
}

//TODO: I want this in netcontroller
void ThreadForm::downloadFile(const QString &fileUrl,
								 const QString &filePath,
								 QNetworkAccessManager *manager,
								 QString message){
	QFile *file = new QFile(filePath);
	if(file->exists()){
		downloadedSlot(filePath,message);
		file->deleteLater();
		return;
	}
	QNetworkRequest request;
	request.setUrl(fileUrl);
	if(api->requiresUserAgent()) request.setHeader(QNetworkRequest::UserAgentHeader,api->requiredUserAgent());
	QNetworkReply *reply;
	if(networkReplies.contains(fileUrl)){
		reply = networkReplies.value(fileUrl);
		qDebug().noquote() << "already downloading" << fileUrl << "to" << filePath;
	}
	else{
		reply = manager->get(request);
		networkReplies.insert(fileUrl,reply);
		qDebug().noquote() << "downloading" << fileUrl << "to" << filePath;
	}
	connect(reply,&QNetworkReply::finished,[=]{
		networkReplies.remove(fileUrl);
		reply->deleteLater();
		file->deleteLater();
		if(!reply) return;
		if(reply->error()){
			qDebug().noquote().nospace() << "error downloading " << fileUrl << ": " << reply->errorString();
		}
		else{
			if(!file->open(QIODevice::WriteOnly)){
				qDebug().noquote().nospace() << "error writing " << filePath << ": Could not open for writing";
				file->close();
				return;
			};
			file->write(reply->readAll());
			file->close();
			downloadedSlot(filePath,message);
		}
	});
}

void ThreadForm::downloadedSlot(const QString &path, const QString &message){
	if(path.compare(strings.flagPath) == 0){
		ui->info->update();
		QListIterator< QPointer<ThreadForm> > i(rootTF->clones);
		QPointer<ThreadForm> cloned;
		while(i.hasNext()) {
			cloned = i.next();
			if(!cloned) continue;
			cloned->strings.countryString = this->strings.countryString;
			cloned->setInfoString();
		}
	}
	else if(path.startsWith("flags/flegs/")){
		if(++regionsGot != regionList.size()) return;
		ui->info->update();
		QListIterator< QPointer<ThreadForm> > i(rootTF->clones);
		QPointer<ThreadForm> cloned;
		while(i.hasNext()) {
			cloned = i.next();
			if(!cloned) continue;
			cloned->strings.regionString = this->strings.regionString;
			cloned->setInfoString();
		}
	}
	else if(path.compare(strings.filePath) == 0){
		gettingFile = false;
		finished = true;
		if(post.ext.compare(".jpg") == 0 || post.ext.compare(".png") == 0) {
			loadImage(strings.filePath);
			QListIterator< QPointer<ThreadForm> > i(rootTF->clones);
			QPointer<ThreadForm> cloned;
			while(i.hasNext()) {
				cloned = i.next();
				if(!cloned) continue;
				cloned->loadImage(strings.filePath);
			}
		}
		if(message.compare("clicked") == 0 || loadIt){
			openImage();
		}
	}
	else if(path.compare(strings.thumbPath) == 0){
		if(!finished) loadImage(strings.thumbPath);
	}
}

void ThreadForm::clickImage(){
	if(QPointer<ClickableLabel>(ui->tim)) ui->tim->clicked();
}

QPixmap ThreadForm::scaleImage(QString path, int scale){
	QImage pic;
	pic.load(path);
	QImage scaled = (pic.height() > pic.width()) ?
				pic.scaledToHeight(scale, Qt::SmoothTransformation) :
				pic.scaledToWidth(scale, Qt::SmoothTransformation);
	return QPixmap::fromImage(scaled);
}

void ThreadForm::onSetPixmap(){
	ui->tim->setPixmap(scaled);
	ui->tim->setFixedSize(scaled.size());
	ui->tim->show();
}

void ThreadForm::loadImage(QString path) {
	QSettings settings(QSettings::IniFormat,QSettings::UserScope,"qtchan","qtchan");
	int maxWidth = settings.value("imageSize",250).toInt();
	QFuture<QPixmap> newImage = QtConcurrent::run(&ThreadForm::scaleImage,path,maxWidth);
	connect(&watcher, &QFutureWatcherBase::finished,[=]()
	{
		scaled = newImage.result();
		emit setPixmap();
	});
	watcher.setFuture(newImage);

	/*QImage scaled = scaleImage(path, settings.value("imageSize",250).toInt());
	if(!scaled.isNull()) {
		ui->tim->show();
		ui->tim->setPixmap(QPixmap::fromImage(scaled));
		ui->tim->setFixedSize(scaled.size());
	}*/

	//Possible to wrap text around image; decided not to for now
	//no smooth scaling doesn't look good
	//or scaling then storing image as base64 data takes too much memory
	//save scaled thumbnails? use qml?
	/*
	QImage scaled = scaleImage(path, settings.value("imageSize",250).toInt());
	QString url;
	if(!scaled.isNull()) {
		QPixmap pixmap = QPixmap::fromImage(scaled);
		QByteArray byteArray;
		QBuffer buffer(&byteArray);
		pixmap.save(&buffer, "JPG");
		url = QString("<img style=\"float:left\" src=\"data:image/jpg;base64,") + byteArray.toBase64() + "\"/>";
	}
	ui->com->setText("<a href=\"#t" % path % "\">" % url % "</a><p>" + post.com + "</p>");
	*/
}

void ThreadForm::imageClicked()
{
	qDebug().noquote().nospace() << "clicked " << post.filename << post.ext;
	if(strings.path == "index") {
		TreeItem *childOf = mw->model->getItem(mw->selectionModel->currentIndex());
		mw->onNewThread(mw,api,strings.board,strings.thread,QString(),childOf);
	}
	else{
		if(finished) openImage();
		else if(gettingFile) loadIt = true;
		else getFile(true);
	}
}

void ThreadForm::hideClicked()
{
	QSettings settings(QSettings::IniFormat,QSettings::UserScope,"qtchan","qtchan");
	//deprecated
	QString filterString = "filters/" % api->name() % '/' % board % "/id";
	QStringList idFilters = settings.value(filterString).toStringList();
	idFilters.append(strings.thread);
	settings.setValue(filterString,idFilters);
	//
	qDebug().noquote().nospace() << "hide Clicked so " << api->name() << '/' << board << '/' << post.no << " filtered!";
	this->hidden = true;
	emit removeMe(this);
	if(strings.thread != "index"){
		QListIterator< QPointer<ThreadForm> > i(clones);
		while(i.hasNext()) {
			i.next()->deleteLater();
		}
		QSet<QString> quotes = post.quotelinks;
		ThreadForm *replyTo;
		//TODO use QtConcurrent?
		foreach (const QString &orig, quotes)
		{
			replyTo = static_cast<ThreadTab*>(tab)->tfMap.find(orig).value();
			if(replyTo != nullptr) {
				replyTo->replies.remove(post.no.toDouble());
				replyTo->setReplies();
			}
		}
	}
}

void ThreadForm::openImage()
{
	QDesktopServices::openUrl(
				QUrl::fromLocalFile(
					QDir().absoluteFilePath(strings.filePath)));
}

void ThreadForm::setFontSize(int fontSize){
	QFont temp = this->ui->info->font();
	temp.setPointSize(fontSize-4);
	this->ui->fileInfo->setFont(temp);
	temp.setPointSize(fontSize-2);
	this->ui->info->setFont(temp);
	temp.setPointSize(fontSize);
	this->ui->com->setFont(temp);
	if(root && clones.size()){
		QListIterator< QPointer<ThreadForm> > i(clones);
		while(i.hasNext()) {
			QPointer<ThreadForm> next = i.next();
			if(!next) continue;
			next->setFontSize(fontSize);
		}
	}
}

void ThreadForm::setImageSize(int imageSize){
	if(file && file->exists()) loadImage(strings.filePath);
	else if(thumb && thumb->exists()) loadImage(strings.thumbPath);
	if(root && clones.size()){
		QListIterator< QPointer<ThreadForm> > i(clones);
		while(i.hasNext()) {
			QPointer<ThreadForm> next = i.next();
			if(!next) continue;
			next->setImageSize(imageSize);
		}
	}
}

//TODO put most of this in another thread
void ThreadForm::setRegion(const QString &region){
	regionList = region.split("||");
	if(regionList.isEmpty()) return;
	regionString = "<span style=\"color:lightblue\">" % region % "</span> ";
	ui->info->setText(infoString());
	QString flegUrl(strings.flegsUrlBase % post.country_name);
	QString flegPath("flags/flegs/" % post.country_name);
	QString temp(regionList.takeLast());
	QDir().mkpath(flegPath % '/' % regionList.join('/'));
	regionList.append(temp);
	int i = 0;
	regionString.clear();
	foreach(QString reg, regionList){
		flegPath += '/' % reg;
		flegUrl += '/' % reg;
		downloadFile(QUrl(flegUrl % ".png").toString(QUrl::FullyEncoded),flegPath % ".png",nc.fileManager);
		//regionString += flagString(flegPath % ".png",reg);
		regionString += "<a href=\"" % reg % "\" style=\"color:lightblue;text-decoration:none\" text><img src=\"" % flegPath % ".png\" width=\"32\" height=\"20\"> " % reg % "</a> ";
		i++;
	}
	ui->info->setText(infoString());
}

void ThreadForm::quoteClicked(const QString &link)
{
	qDebug().noquote() << link;
	if(link.startsWith("#p") && this->type == PostType::Reply) {
		QString postNo = link.mid(2);
		if(inserted.contains(postNo)){
			inserted.value(postNo)->deleteLater();
		}
		else{
			ThreadForm *tf = static_cast<ThreadTab*>(tab)->tfMap.value(postNo);
			if(tf != nullptr && !tf->hidden) this->insert(tf);
		}
	}
	else if(link.startsWith("#op")){
		if(this->type == PostType::Reply) static_cast<ThreadTab*>(tab)->quoteIt(">>"+post.no);
		else imageClicked();
	}
	else if(link.startsWith("#f")){
		postMenu();
	}
	else if(!link.isEmpty() && link.at(0)=='/') {
		mw->loadFromSearch(link,QString(),Q_NULLPTR,false);
	}
}

void ThreadForm::postMenu(){
	//tfc will delete on close
	ThreadFormContext *tfc = new ThreadFormContext(&post);
	connect(tfc,&ThreadFormContext::filtersChanged,mw,&MainWindow::reloadFilters,Qt::DirectConnection);
}

void ThreadForm::insert(ThreadForm *tf)
{
	ThreadForm *newtf = tf->clone(replyLevel);
	inserted.insert(tf->post.no,newtf);
	QMetaObject::Connection removeOnDestroy = connect(newtf,&ThreadForm::destroyed,this,&ThreadForm::removeFromInserted);
	insertedConnections.insert(tf->post.no,removeOnDestroy);
	if(ui->quoteWidget->isHidden())ui->quoteWidget->show();
	ui->quotes->addWidget(newtf);
}

void ThreadForm::removeFromInserted(){
	ThreadForm *tf = static_cast<ThreadForm*>(sender());
	QString postNo = tf->post.no;
	if(!postNo.isEmpty() && inserted.contains(postNo)){
		inserted.remove(postNo);
		insertedConnections.remove(postNo);
	}
}

void ThreadForm::addReply(ThreadForm *tf){
	if(ui->quoteWidget->isHidden()) ui->quoteWidget->show();
	tf->setParent(this);
	ui->quotes->addWidget(tf);
}

ThreadForm *ThreadForm::clone(int replyLevel)
{
	ThreadForm *tfs = new ThreadForm(this->api,this->strings,false,false,tab,replyLevel+1);
	tfs->rootTF = this->rootTF;
	tfs->tab = tab;
	tfs->post = this->post;
	tfs->ui->com->setText(post.com);
	tfs->ui->info->setText(ui->info->text());
	tfs->replies = replies;
	//TODO check and account for if original is still getting file
	if(!post.tim.isNull() && !post.filedeleted) {
		tfs->strings = this->strings;
		tfs->file = file;
		tfs->thumb = thumb;
		tfs->ui->fileInfo->show();
		tfs->ui->fileInfo->setText(ui->fileInfo->text());
		const QPixmap *px = this->ui->tim->pixmap();
		//From load image but don't have to scale again
		tfs->ui->tim->show();
		//TODO make sure clone fits in window
		//tfs->setMinimumWidth(738);
		if(px){
			tfs->ui->tim->setPixmap(*px);
			tfs->ui->tim->setFixedSize(px->size());
			connect(tfs->ui->tim,&ClickableLabel::clicked,rootTF,&ThreadForm::imageClicked);
		}
	} else {
		tfs->ui->pictureLayout->deleteLater();
		tfs->hasImage = false;
	}
	tfs->regionString = regionString;
	if(repliesString.length()) {
		tfs->setRepliesString(repliesString);
	}
	rootTF->clones.append(tfs);
	disconnect(tfs->ui->hide,&ClickableLabel::clicked,tfs,&ThreadForm::hideClicked);
	connect(tfs->ui->hide,&ClickableLabel::clicked,tfs,&ThreadForm::deleteLater);
	connect(tfs,&ThreadForm::removeMe,rootTF,&ThreadForm::removeClone);
	//TODO load and connect cross thread replies
	if(this->type == PostType::Reply){
		ThreadTab* temp = static_cast<ThreadTab*>(tab);
		connect(tfs,&ThreadForm::floatLink,temp,&ThreadTab::floatReply);
		connect(tfs,&ThreadForm::updateFloat,temp,&ThreadTab::updateFloat);
		connect(tfs,&ThreadForm::deleteFloat,temp,&ThreadTab::deleteFloat);
	}
	return tfs;
}

//TODO don't run this if destroying the whole threadtab
void ThreadForm::removeClone(QPointer<ThreadForm> tf)
{
	if(tf) clones.removeOne(tf);
}
void ThreadForm::addReplyLink(QString &reply, bool isYou){
	QString temp =" <a href=\"#p" % reply % "\" style=\"color:#897399\">&gt;&gt;" % reply % ((isYou) ? " (You)</a>" : "</a>");
	repliesString += temp;
	ui->info->setText(infoString());
	//update clones
	QListIterator< QPointer<ThreadForm> > i(clones);
	while(i.hasNext()) {
		QPointer<ThreadForm> next = i.next();
		if(!next) continue;
		next->setRepliesString(repliesString);
	}
}

void ThreadForm::setReplies()
{
	repliesString = "";
	QList<QString> list = replies.values();
	if(list.length()) {
		foreach (const QString &reply, list)
		{
			repliesString+=" <a href=\"#p" % reply % "\" style=\"color:#897399\">>>" % reply % (you.hasYou(api->name(),board,reply) ? " (You)</a>" : "</a>");
		}
		repliesString = repliesString.mid(1);
		ui->info->setText(infoString());
		//update clones
		QListIterator< QPointer<ThreadForm> > i(clones);
		while(i.hasNext()) {
			QPointer<ThreadForm> next = i.next();
			if(!next) continue;
			next->repliesString = repliesString;
			next->setInfoString();
		}
	}
	else{
		//ui->replies->hide();
	}
}

void ThreadForm::setRepliesString(const QString &repliesString)
{
	this->repliesString = repliesString;
	setInfoString();
}

void ThreadForm::setInfoString()
{
	ui->info->setText(infoString());
}

//showing and supposedToShow to save states and restore floating replies and quotes
void ThreadForm::on_info_linkHovered(const QString &link)
{
	if(this->type == PostType::Reply) {
		if(link.startsWith("#op")){
			emit deleteFloat();
		}
		else if(link.startsWith("#p")) {
			qDebug().noquote() << "hovering" << link;
			emit floatLink(link.mid(2),replyLevel);
		} else {
			//TODO check mouse cursor?
			emit deleteFloat();
		}
	}
}

bool ThreadForm::eventFilter(QObject *obj, QEvent *event)
{
	if(this->type == PostType::Reply){
		if(event->type() == QEvent::MouseMove) {
			emit updateFloat();
		}
		else if(event->type() == QEvent::Leave) {
			emit deleteFloat();
		}
	}
	if((event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease)
			&& (obj->objectName() == "com" || obj->objectName() == "info" || obj->objectName() == "fileInfo")){
		QLabel *temp = static_cast<QLabel*>(obj);
		if(temp->hasSelectedText()){
			temp->setTextInteractionFlags(
				Qt::TextSelectableByKeyboard |
				Qt::TextSelectableByMouse |
				Qt::LinksAccessibleByKeyboard |
				Qt::LinksAccessibleByMouse);
		}
		else{
			temp->setTextInteractionFlags(
				Qt::TextSelectableByMouse |
				Qt::LinksAccessibleByKeyboard |
				Qt::LinksAccessibleByMouse);
			ui->hide->setFocus();
			temp->setFocus();
		}
	}
	return QObject::eventFilter(obj, event);
}

void ThreadForm::on_com_linkHovered(const QString &link)
{
	on_info_linkHovered(link);
}

void ThreadForm::deleteHideLayout()
{
	ui->hideLayout->deleteLater();
	ui->quoteWidget->deleteLater();
	hideButtonShown = false;
}

void ThreadForm::paintEvent(QPaintEvent *){
	QPainter painter(this);
	int x = 0;
	if(hideButtonShown){
		painter.setPen(QColor(100,100,100));
		x = ui->hide->width();
	}
	else
		painter.setPen(QColor(75,75,75));
	painter.drawRect(x,0,width()-x-1,height()-1);
	painter.fillRect(x+1,1,width()-x-2,height()-2,background);
}
