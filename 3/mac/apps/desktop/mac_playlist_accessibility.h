#pragma once

#ifdef Q_OS_MACOS
#include <QAccessibleWidget>
#include <QHash>
#include <QListWidget>
#include <QPersistentModelIndex>
#include <QPointer>

namespace vtrpon {
// Expose this one-column list as real list items. Qt's Cocoa synthetic table
// rows can invalidate cached cell interfaces during AXSelectedChildren reads.
class MacPlaylistAccessibleItem final : public QAccessibleInterface,
                                        public QAccessibleActionInterface {
public:
    MacPlaylistAccessibleItem(QListWidget *list, const QModelIndex &index) : list_(list), index_(index) {}
    bool isValid() const override { return list_ && index_.isValid(); }
    QObject *object() const override { return nullptr; }
    QAccessibleInterface *parent() const override { return list_ ? QAccessible::queryAccessibleInterface(list_) : nullptr; }
    QAccessibleInterface *child(int) const override { return nullptr; }
    QAccessibleInterface *childAt(int, int) const override { return nullptr; }
    int childCount() const override { return 0; }
    int indexOfChild(const QAccessibleInterface *) const override { return -1; }
    QString text(QAccessible::Text type) const override {
        if (!isValid()) return {};
        if (type == QAccessible::Name) {
            auto name = index_.data(Qt::AccessibleTextRole).toString();
            return name.isEmpty() ? index_.data(Qt::DisplayRole).toString() : name;
        }
        return type == QAccessible::Description ? index_.data(Qt::AccessibleDescriptionRole).toString() : QString{};
    }
    void setText(QAccessible::Text, const QString &) override {}
    QRect rect() const override {
        if (!isValid()) return {};
        const auto local = list_->visualItemRect(list_->item(index_.row()));
        return QRect(list_->viewport()->mapToGlobal(local.topLeft()), local.size());
    }
    QAccessible::Role role() const override { return QAccessible::ListItem; }
    QAccessible::State state() const override {
        QAccessible::State result;
        result.invalid = !isValid();
        if (isValid()) {
            result.selectable = true;
            result.focusable = true;
            result.selected = list_->item(index_.row())->isSelected();
            result.focused = list_->hasFocus() && list_->currentRow() == index_.row();
            result.disabled = !list_->isEnabled();
            result.invisible = !list_->isVisible();
        }
        return result;
    }
    void *interface_cast(QAccessible::InterfaceType type) override {
        return type == QAccessible::ActionInterface ? static_cast<QAccessibleActionInterface *>(this) : nullptr;
    }
    QStringList actionNames() const override { return {pressAction(), setFocusAction()}; }
    void doAction(const QString &action) override {
        if (isValid() && (action == pressAction() || action == setFocusAction())) {
            list_->setCurrentRow(index_.row(), QItemSelectionModel::ClearAndSelect);
            list_->setFocus();
        }
    }
    QStringList keyBindingsForAction(const QString &) const override { return {}; }
    int row() const { return isValid() ? index_.row() : -1; }
private:
    QPointer<QListWidget> list_;
    QPersistentModelIndex index_;
};

class MacPlaylistAccessibleList final : public QAccessibleWidget,
                                        public QAccessibleSelectionInterface {
public:
    explicit MacPlaylistAccessibleList(QListWidget *list) : QAccessibleWidget(list, QAccessible::List), list_(list) {}
    ~MacPlaylistAccessibleList() override {
        for (auto id : items_) QAccessible::deleteAccessibleInterface(id);
    }
    int childCount() const override { return list_ ? list_->count() : 0; }
    QAccessibleInterface *child(int row) const override {
        if (row < 0 || row >= childCount()) return nullptr;
        const QPersistentModelIndex index(list_->model()->index(row, 0));
        auto id = items_.value(index);
        if (auto *item = QAccessible::accessibleInterface(id)) return item;
        auto *item = new MacPlaylistAccessibleItem(list_, index);
        items_.insert(index, QAccessible::registerAccessibleInterface(item));
        return item;
    }
    int indexOfChild(const QAccessibleInterface *item) const override {
        return item && item->parent() == this ? static_cast<const MacPlaylistAccessibleItem *>(item)->row() : -1;
    }
    QAccessibleInterface *childAt(int x, int y) const override {
        if (!list_) return nullptr;
        return child(list_->indexAt(list_->viewport()->mapFromGlobal(QPoint(x, y))).row());
    }
    QAccessibleInterface *focusChild() const override { return list_ && list_->hasFocus() ? child(list_->currentRow()) : nullptr; }
    void *interface_cast(QAccessible::InterfaceType type) override {
        if (type == QAccessible::SelectionInterface) return static_cast<QAccessibleSelectionInterface *>(this);
        return QAccessibleWidget::interface_cast(type);
    }
    int selectedItemCount() const override { return list_ ? list_->selectedItems().size() : 0; }
    QList<QAccessibleInterface *> selectedItems() const override {
        QList<QAccessibleInterface *> result;
        if (list_) for (auto *item : list_->selectedItems()) result.append(child(list_->row(item)));
        return result;
    }
    bool select(QAccessibleInterface *item) override {
        const int row = indexOfChild(item);
        if (row < 0) return false;
        list_->setCurrentRow(row, QItemSelectionModel::ClearAndSelect);
        return true;
    }
    bool unselect(QAccessibleInterface *item) override {
        const int row = indexOfChild(item);
        if (row < 0) return false;
        list_->item(row)->setSelected(false);
        return true;
    }
    bool selectAll() override { return select(child(list_ && list_->currentRow() >= 0 ? list_->currentRow() : 0)); }
    bool clear() override { if (!list_) return false; list_->clearSelection(); return true; }
private:
    QPointer<QListWidget> list_;
    mutable QHash<QPersistentModelIndex, QAccessible::Id> items_;
};

inline void installMacPlaylistAccessibility(QListWidget *list) {
    static const bool installed = [] {
        QAccessible::installFactory([](const QString &, QObject *object) -> QAccessibleInterface * {
            auto *widget = qobject_cast<QListWidget *>(object);
            return widget && widget->property("vtrponAccessiblePlaylist").toBool()
                ? new MacPlaylistAccessibleList(widget) : nullptr;
        });
        return true;
    }();
    (void)installed;
    list->setProperty("vtrponAccessiblePlaylist", true);
}
}
#endif
