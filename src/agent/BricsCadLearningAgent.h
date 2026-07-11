#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

class BricsCadLearningAgent {
public:
    void setProjectRootPath(const QString& path);
    bool load(QString* errorMessage = nullptr);

    QJsonObject document() const;
    QJsonObject metadata() const;
    QJsonObject policy() const;
    QString sourcePath() const;
    bool loadedFromProjectFile() const;

    QJsonObject toolProfile(const QString& name) const;
    QJsonArray toolProfiles() const;

    QJsonArray learningIndex() const;
    QJsonArray lessonIndex() const;
    QJsonObject lessonById(const QString& id) const;
    QJsonArray relevantLessons(const QString& prompt, int maxCount = 3) const;
    QJsonObject contextForPrompt(const QString& prompt, int maxLessons = 3,
        const QString& selectedWorkflowId = {}, const QStringList& selectedTools = {}) const;

    bool applyLearningUpdate(const QJsonObject& update, QStringList* appliedChanges = nullptr, QString* errorMessage = nullptr);
    bool recordLessonUse(const QJsonArray& lessonIds, const QJsonObject& event, QStringList* appliedChanges = nullptr, QString* errorMessage = nullptr);
    bool upsertRuntimeLessonFromEvent(const QJsonObject& event, QStringList* affectedLessonIds = nullptr, QStringList* appliedChanges = nullptr, QString* errorMessage = nullptr);
    bool deprecateLesson(const QString& id, QString* errorMessage = nullptr);
    bool preferLesson(const QString& id, QString* errorMessage = nullptr);

private:
    QString projectLearningPath() const;
    bool loadFromPath(const QString& path, QJsonObject* document, QString* errorMessage = nullptr) const;
    bool saveProjectDocument(QString* errorMessage = nullptr) const;
    void rebuildIndexes();

    static QString normalizedText(QString text);
    static QString lessonSearchText(const QJsonObject& lesson);
    static QJsonObject compactLesson(const QJsonObject& lesson, bool detailed);
    static QJsonObject compactToolProfile(const QJsonObject& profile);
    static QJsonObject compactRepairRule(const QJsonObject& rule, const QJsonObject& metadata);
    static QJsonObject compactExample(const QJsonObject& example, const QJsonObject& metadata);
    static int scoreLesson(const QJsonObject& lesson, const QStringList& terms, const QString& normalizedPrompt);
    QString generatedLessonId(const QJsonObject& lesson, const QJsonArray& lessons) const;
    int matchingLessonIndex(const QJsonObject& lesson, const QJsonArray& lessons) const;
    bool upsertLesson(QJsonObject lesson, QString* appliedChange, QString* errorMessage);
    bool upsertRepairRule(QJsonObject rule, QString* appliedChange, QString* errorMessage);
    bool appendSuccessfulExample(QJsonObject example, QString* appliedChange, QString* errorMessage);
    void refreshMetadata();

    QString m_projectRootPath;
    QString m_sourcePath;
    bool m_loadedFromProjectFile = false;
    QJsonObject m_document;
    QJsonObject m_toolProfilesByName;
    QJsonObject m_lessonsById;
};
