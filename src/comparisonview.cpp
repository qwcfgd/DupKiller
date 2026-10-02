#include "comparisonview.h"
#include <QComboBox>
#include <QEvent>
#include <QFrame>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QPainter>
#include <QPointer>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QStyle>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>

namespace dup {
namespace {
class KeeperDelegate final : public QStyledItemDelegate {
public:
    explicit KeeperDelegate(ComparisonModel *model, QObject *parent) : QStyledItemDelegate(parent), model(model) {}
    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &, const QModelIndex &i) const override {
        if (!(i.flags() & Qt::ItemIsEditable)) return nullptr;
        auto *combo = new QComboBox(parent); combo->setObjectName("keeperChoice");
        combo->setSizeAdjustPolicy(QComboBox::AdjustToContents); combo->setMinimumContentsLength(35);
        for (const auto &choice : model->keeperChoices(i)) combo->addItem(choice.first, choice.second);
        auto *self = const_cast<KeeperDelegate *>(this);
        QObject::connect(combo, QOverload<int>::of(&QComboBox::activated), self, [self, combo] { const QPointer<QComboBox> guard(combo); emit self->commitData(combo); if (guard) emit self->closeEditor(guard.data()); });
        QTimer::singleShot(0, combo, [combo] { combo->showPopup(); }); return combo;
    }
    void setEditorData(QWidget *editor, const QModelIndex &i) const override {
        auto *combo = qobject_cast<QComboBox *>(editor); if (combo) combo->setCurrentIndex(combo->findData(i.data(Qt::EditRole)));
    }
    void setModelData(QWidget *editor, QAbstractItemModel *m, const QModelIndex &i) const override {
        auto *combo = qobject_cast<QComboBox *>(editor); if (combo) m->setData(i, combo->currentData(), Qt::EditRole);
    }
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &i) const override {
        QStyledItemDelegate::paint(painter, option, i);
        if (!(i.flags() & Qt::ItemIsEditable)) return;
        QStyleOption arrow; arrow.rect = QRect(option.rect.right() - 18, option.rect.center().y() - 4, 9, 9); arrow.state = QStyle::State_Enabled;
        if (option.widget) option.widget->style()->drawPrimitive(QStyle::PE_IndicatorArrowDown, &arrow, painter, option.widget);
    }
private:
    ComparisonModel *model;
};
}
ComparisonView::ComparisonView(ComparisonModel *model, QWidget *parent) : QWidget(parent), model(model) {
    setObjectName("comparisonView"); auto *layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0);
    splitter = new QSplitter(Qt::Horizontal, this); splitter->setObjectName("comparisonSplitter"); splitter->setChildrenCollapsible(false); splitter->setHandleWidth(8);
    original = new QTreeView; candidate = new QTreeView; originalRoot = new QTreeView; candidateRoot = new QTreeView;
    original->setObjectName("originalTree"); candidate->setObjectName("fileTree"); originalRoot->setObjectName("originalRootRow"); candidateRoot->setObjectName("candidateRootRow");
    auto *selection = new QItemSelectionModel(model, this);
    for (auto *view : {original, candidate, originalRoot, candidateRoot}) { view->setModel(model); view->setSelectionModel(selection); }
    auto makePane = [&](bool left, QTreeView *body, QTreeView *frozen) {
        auto *pane = new QFrame; pane->setObjectName("comparisonPane"); pane->setMinimumWidth(280);
        auto *column = new QVBoxLayout(pane); column->setContentsMargins(0, 0, 0, 0); column->setSpacing(0);
        auto *title = new QLabel(left ? QStringLiteral("原始文件 · 保留原件") : QStringLiteral("待替换文件 · 快捷方式 → .lnk"));
        title->setStyleSheet(left ? "padding:8px 12px;background:#e7f3ed;color:#13755d;font-weight:bold;" : "padding:8px 12px;background:#fff2e4;color:#985115;font-weight:bold;");
        column->addWidget(title); column->addWidget(frozen); column->addWidget(body, 1); splitter->addWidget(pane);
        bindPane(body, frozen, left);
    };
    makePane(true, original, originalRoot); makePane(false, candidate, candidateRoot); layout->addWidget(splitter);
    splitter->setStretchFactor(0, 1); splitter->setStretchFactor(1, 1); splitter->setSizes({650, 650});
    original->setItemDelegateForColumn(ComparisonModel::OriginalName, new KeeperDelegate(model, original));
    connect(original, &QTreeView::clicked, this, [this](const QModelIndex &i) { if (i.column() == ComparisonModel::OriginalName && (i.flags() & Qt::ItemIsEditable)) original->edit(i); });
    connect(original->verticalScrollBar(), &QScrollBar::valueChanged, candidate->verticalScrollBar(), &QScrollBar::setValue);
    connect(candidate->verticalScrollBar(), &QScrollBar::valueChanged, original->verticalScrollBar(), &QScrollBar::setValue);
    connect(original, &QTreeView::expanded, candidate, &QTreeView::expand); connect(candidate, &QTreeView::expanded, original, &QTreeView::expand);
    connect(original, &QTreeView::collapsed, candidate, &QTreeView::collapse); connect(candidate, &QTreeView::collapsed, original, &QTreeView::collapse);
    connect(model, &QAbstractItemModel::modelReset, this, &ComparisonView::restoreTree);
    connect(model, &QAbstractItemModel::layoutChanged, this, [this] { const auto value = candidate->verticalScrollBar()->value(); original->verticalScrollBar()->setValue(value); });
    restoreTree(); setSort(ComparisonModel::CandidateName, Qt::AscendingOrder);
}
void ComparisonView::bindPane(QTreeView *body, QTreeView *frozen, bool left) {
    for (auto *view : {body, frozen}) {
        view->setStyleSheet("QTreeView{border:0;border-radius:0;} QTreeView::item{height:29px;}");
        view->setUniformRowHeights(true); view->setWordWrap(false); view->setAllColumnsShowFocus(true);
        view->setSelectionMode(QAbstractItemView::ExtendedSelection); view->setSelectionBehavior(QAbstractItemView::SelectRows);
        view->header()->setSectionsMovable(false); view->header()->setStretchLastSection(false); view->header()->setMinimumSectionSize(55);
        view->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel); view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        view->setTreePosition(left ? ComparisonModel::OriginalName : ComparisonModel::CandidateName);
        view->setProperty("originalSide", left); view->viewport()->setProperty("originalSide", left);
        view->header()->setProperty("originalSide", left);
        view->installEventFilter(this); view->viewport()->installEventFilter(this); view->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(view, &QTreeView::customContextMenuRequested, this, [this, view](const QPoint &p) { emit contextRequested(view, p); });
        for (int c = 0; c < ComparisonModel::ColumnCount; ++c) view->setColumnHidden(c, left ? c >= ComparisonModel::CandidateName : c < ComparisonModel::CandidateName);
    }
    body->setAlternatingRowColors(true); body->setHeaderHidden(true); body->setIndentation(18); body->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    body->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn); body->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    frozen->setRootIsDecorated(false); frozen->setItemsExpandable(false); frozen->setEditTriggers(QAbstractItemView::NoEditTriggers);
    // Equal viewport widths keep the frozen header and body columns aligned.
    frozen->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn); frozen->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    frozen->setFixedHeight(frozen->header()->sizeHint().height() + 33); frozen->header()->setSortIndicatorShown(true);
    frozen->header()->installEventFilter(this);
    const int widths[] = {225, 190, 88, 155, 70, 225, 180, 90, 88, 155, 70};
    for (int c = 0; c < ComparisonModel::ColumnCount; ++c) { frozen->setColumnWidth(c, widths[c]); body->setColumnWidth(c, widths[c]); }
    connect(frozen->header(), &QHeaderView::sectionResized, this, [this, body](int column, int, int width) { if (!resizing) { resizing = true; body->setColumnWidth(column, width); resizing = false; } });
    connect(body->horizontalScrollBar(), &QScrollBar::valueChanged, frozen->horizontalScrollBar(), &QScrollBar::setValue);
    connect(frozen->header(), &QHeaderView::sortIndicatorChanged, this, [this](int column, Qt::SortOrder order) { setSort(column, order); });
}
bool ComparisonView::eventFilter(QObject *object, QEvent *event) {
    if (event->type() == QEvent::FocusIn || event->type() == QEvent::MouseButtonPress) selectingOriginal = object->property("originalSide").toBool();
    if ((object == originalRoot || object == candidateRoot || object == originalRoot->header() || object == candidateRoot->header()) &&
        (event->type() == QEvent::Resize || event->type() == QEvent::StyleChange || event->type() == QEvent::FontChange)) updateFrozenHeights();
    return QWidget::eventFilter(object, event);
}
QModelIndexList ComparisonView::sourceSelection(bool candidates) const {
    const bool left = !candidates && selectingOriginal;
    QModelIndexList selected;
    for (const auto &i : candidate->selectionModel()->selectedRows(left ? ComparisonModel::OriginalName : ComparisonModel::CandidateName)) {
        const auto mapped = model->sourceIndex(i, left); if (mapped.isValid() && !selected.contains(mapped)) selected.append(mapped);
    }
    return selected;
}
void ComparisonView::setSort(int column, Qt::SortOrder order) {
    const QSignalBlocker a(originalRoot->header()), b(candidateRoot->header());
    const int originalColumn = column < ComparisonModel::CandidateName ? column
        : column == ComparisonModel::CandidateName ? ComparisonModel::OriginalName
        : column == ComparisonModel::CandidateDirectory ? ComparisonModel::OriginalDirectory
        : column == ComparisonModel::CandidateModified ? ComparisonModel::OriginalModified
        : column == ComparisonModel::CandidateType ? ComparisonModel::OriginalType : ComparisonModel::OriginalSize;
    const int candidateColumn = column >= ComparisonModel::CandidateName ? column
        : column == ComparisonModel::OriginalName ? ComparisonModel::CandidateName
        : column == ComparisonModel::OriginalDirectory ? ComparisonModel::CandidateDirectory
        : column == ComparisonModel::OriginalModified ? ComparisonModel::CandidateModified
        : column == ComparisonModel::OriginalType ? ComparisonModel::CandidateType : ComparisonModel::CandidateSize;
    originalRoot->header()->setSortIndicator(originalColumn, order); candidateRoot->header()->setSortIndicator(candidateColumn, order); model->sort(column, order);
}
void ComparisonView::restoreTree() {
    const auto folder = model->index(0, 0); original->setRootIndex(folder); candidate->setRootIndex(folder);
    if (model->pairCount() <= 1500) { original->expandAll(); candidate->expandAll(); }
    else { original->expandToDepth(1); candidate->expandToDepth(1); }
    originalRoot->collapseAll(); candidateRoot->collapseAll();
    updateFrozenHeights();
}
void ComparisonView::updateFrozenHeights() {
    if (heightUpdateQueued) return;
    heightUpdateQueued = true;
    QTimer::singleShot(0, this, [this] {
        heightUpdateQueued = false;
        const int height = qMax(originalRoot->header()->height(), candidateRoot->header()->height()) +
            qMax(33, qMax(originalRoot->sizeHintForRow(0), candidateRoot->sizeHintForRow(0))) + 2;
        originalRoot->setFixedHeight(height); candidateRoot->setFixedHeight(height);
    });
}
}
