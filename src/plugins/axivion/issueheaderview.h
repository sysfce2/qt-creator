// Copyright (C) 2023 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#pragma once

#include <QHeaderView>
#include <QList>

namespace Axivion::Internal {

enum class SortOrder { None, Ascending, Descending };

class IssueHeaderView : public QHeaderView
{
    Q_OBJECT
public:
    explicit IssueHeaderView(QWidget *parent = nullptr) : QHeaderView(Qt::Horizontal, parent) {}
    void setSortableColumns(const QList<bool> &sortable);
    void setColumnWidths(const QList<int> &widths) { m_columnWidths = widths; }

    SortOrder currentSortOrder() const { return m_currentSortOrder; }
    int currentSortColumn() const;

signals:
    void sortTriggered();

protected:
    void paintSection(QPainter *painter, const QRect &rect, int logicalIndex) const override;
    QSize sectionSizeFromContents(int logicalIndex) const override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;

private:
    void onToggleSort(int index, SortOrder order);
    bool m_dragging = false;
    bool m_maybeToggleSort = false;
    int m_lastToggleLogicalPos = -1;
    int m_currentSortIndex = -1;
    SortOrder m_currentSortOrder = SortOrder::None;
    QList<bool> m_sortableColumns;
    QList<int> m_columnWidths;
};

} // namespace Axivion::Internal
