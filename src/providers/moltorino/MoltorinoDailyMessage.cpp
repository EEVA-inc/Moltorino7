#include "providers/moltorino/MoltorinoDailyMessage.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitContainer.hpp"
#include "widgets/Window.hpp"

#include <QDateTime>
#include <QRandomGenerator>

#include <algorithm>
#include <array>
#include <limits>

namespace {

using namespace chatterino;

constexpr std::array<QStringView, 10> MORNING_LEAD_INS{
    u"Good morning",
    u"A morning thought",
    u"To start the day",
    u"Before you get going",
    u"First thought of the day",
    u"One for the morning",
    u"Starting today",
    u"Morning reading",
    u"For the day ahead",
    u"Before the day gets busy",
};

constexpr std::array<QStringView, 10> AFTERNOON_LEAD_INS{
    u"A note for today",        u"Today's thought",
    u"Worth a thought",         u"While you're here",
    u"One for the afternoon",   u"An afternoon reminder",
    u"A thought to take along", u"Something to keep in mind",
    u"For the rest of the day", u"A moment between things",
};

constexpr std::array<QStringView, 10> EVENING_LEAD_INS{
    u"Good evening",
    u"One for this evening",
    u"Before the day is done",
    u"An evening thought",
    u"As the day winds down",
    u"Something for tonight",
    u"A thought before you go",
    u"For the evening",
    u"Still time for a thought",
    u"A note to end the day",
};

constexpr std::array<QStringView, 10> NEW_DAY_LEAD_INS{
    u"A new day",
    u"Past midnight",
    u"One for the new day",
    u"For the hours ahead",
    u"The first thought today",
    u"For the late hours",
    u"A thought after midnight",
    u"Another day begins",
    u"If you're still awake",
    u"Midnight reading",
};

constexpr std::array<QStringView, 320> DAILY_QUOTES{
    u"Small progress still changes the shape of tomorrow.",
    u"You do not have to rush to be moving forward.",
    u"A gentle step is still a real step.",
    u"There is room in today for something good.",
    u"You have made it through every difficult day behind you.",
    u"Rest is part of the work, not a reward for finishing it.",
    u"Your pace is allowed to look like your own.",
    u"A fresh start can begin in the middle of a day.",
    u"You are worth the patience you give to everyone else.",
    u"Today only asks you to begin where you are.",
    u"Quiet effort grows into visible change.",
    u"You can be proud of progress that nobody else saw.",
    u"Good things can take their time and still arrive.",
    u"One kind choice can soften an entire day.",
    u"You are allowed to make today lighter.",
    u"The next small thing is enough for now.",
    u"Not every good day has to be a productive one.",
    u"Your best can be different from one day to the next.",
    u"There is strength in choosing peace where you can.",
    u"A little hope is still enough to guide a step.",
    u"You can grow without being unkind to who you are now.",
    u"Today is another chance to notice what is going right.",
    u"You deserve moments that ask nothing from you.",
    u"A pause can help you hear what you need.",
    u"The work you do quietly still matters.",
    u"Someone else's timeline does not have to be yours.",
    u"Something does not need to be perfect to be meaningful.",
    u"A calm mind can begin with one slow breath.",
    u"Give yourself credit for choosing to keep going.",
    u"There is more ahead than what feels heavy right now.",
    u"You can carry hope without having every answer.",
    u"The smallest win is still yours to celebrate.",
    u"Your presence makes more of a difference than you can see.",
    u"You are allowed to protect the peace you have built.",
    u"Kindness toward yourself is never wasted.",
    u"A difficult moment does not get to define the whole day.",
    u"You can begin again without calling the first try a failure.",
    u"Some progress feels like courage before it feels like success.",
    u"Let today be steady instead of perfect.",
    u"You have permission to enjoy what is already here.",
    u"There is no shame in needing a softer day.",
    u"A clear boundary can be an act of kindness.",
    u"You can take your time and still trust yourself.",
    u"The future is built from ordinary moments like this one.",
    u"You are learning even when the lesson feels slow.",
    u"A little curiosity can open a very big door.",
    u"It is okay to be a work in progress and a whole person.",
    u"Today can hold both effort and ease.",
    u"Your courage counts even when your voice shakes.",
    u"You can leave room for a pleasant surprise.",
    u"What feels small today may be the start of something lasting.",
    u"You deserve the same grace you offer other people.",
    u"A slower chapter is still part of the story.",
    u"You can notice the hard parts without forgetting the good ones.",
    u"Choosing one thing with care is enough.",
    u"The day does not have to be loud to be beautiful.",
    u"You are allowed to outgrow what no longer fits.",
    u"A kind thought can be a place to rest.",
    u"There is strength in asking for what you need.",
    u"Your effort matters before the result arrives.",
    u"You can trust the person you are becoming.",
    u"Some days are for blooming; some are for growing roots.",
    u"You do not need to earn a moment of peace.",
    u"A hopeful choice is still brave when you feel uncertain.",
    u"You can make space for joy without solving everything first.",
    u"The version of you who kept trying deserves thanks.",
    u"A good life is made from many small, honest moments.",
    u"You are allowed to change your mind with new understanding.",
    u"Let one good thing be enough to notice today.",
    u"There is dignity in starting small.",
    u"You can meet yourself with warmth instead of judgment.",
    u"Your next chapter does not have to look like the last one.",
    u"A little rest can return color to the day.",
    u"The path can be unclear and still lead somewhere good.",
    u"You are more than the task in front of you.",
    u"A patient beginning can become a strong foundation.",
    u"You can be gentle and still be determined.",
    u"The care you put into small things is never invisible.",
    u"Today is allowed to surprise you in a good way.",
    u"A peaceful choice is a meaningful kind of progress.",
    u"You can keep the lesson and release the weight.",
    u"Your life does not need to look impressive to feel worthwhile.",
    u"An honest try is something to be proud of.",
    u"You are allowed to celebrate before everything is finished.",
    u"There is courage in making room for happiness.",
    u"You can take the next step without seeing the whole staircase.",
    u"The good you give has a way of continuing beyond you.",
    u"A new perspective can begin with a quiet moment.",
    u"You do not need a perfect plan to choose a hopeful direction.",
    u"Your small acts of care are building something real.",
    u"Today is not a test; it is a place to live.",
    u"You can have big dreams without letting them rush you.",
    u"There is still time for the day to become kinder.",
    u"You are allowed to feel proud of simply staying present.",
    u"A soft heart and a strong spirit can live together.",
    u"Tomorrow will meet a version of you shaped by today's courage.",
    u"The light you need may already be finding its way in.",
    u"Even a quiet day can leave something beautiful behind.",
    u"Keep a little room for wonder.",
    u"Your kindness has a longer reach than you know.",
    u"Today can be simple and still be good.",
    u"Every steady breath is a small return to yourself.",
    u"You are capable of meeting this moment.",
    u"A warm beginning does not need to be a big one.",
    u"Notice how far you have come without rushing past it.",
    u"Peace can begin with what you choose not to carry.",
    u"There is something hopeful in trying once more.",
    u"Your ordinary days are part of a meaningful life.",
    u"Let the next hour be kinder than the last.",
    u"The world is better for the care you bring to it.",
    u"A small spark can be enough to begin.",
    u"You are still becoming, and that is a beautiful thing.",
    u"Make room for the possibility that things can go well.",
    u"The courage to continue can be quiet.",
    u"Today has not run out of good moments.",
    u"Your gentleness is not a weakness.",
    u"It is enough to move with care.",
    u"A little patience can change the way a day feels.",
    u"Hope often arrives in ordinary clothes.",
    u"You can honor how far you have come.",
    u"Let yourself enjoy the progress you once wished for.",
    u"One peaceful moment can make space for another.",
    u"You bring something to the world that nobody else can.",
    u"The next beginning is closer than it feels.",
    u"There is comfort in taking life one moment at a time.",
    u"Trust that small efforts are adding up.",
    u"You do not need to hurry through the good parts.",
    u"Your story still has room for unexpected joy.",
    u"A steady heart can carry you through uncertain moments.",
    u"Let today contain something just for you.",
    u"The care you give yourself helps everything else grow.",
    u"One honest step can clear a path forward.",
    u"You are allowed to feel hopeful before you have proof.",
    u"The day can change direction at any moment.",
    u"There is beauty in showing up as you are.",
    u"Your future can hold things you have not imagined yet.",
    u"Take the win, even if it arrived quietly.",
    u"You can choose what deserves your energy today.",
    u"A softer approach can still take you somewhere strong.",
    u"Let this moment be a fresh place to stand.",
    u"There are good things worth staying curious about.",
    u"Your patience with yourself is helping you grow.",
    u"A little courage can carry a lot of hope.",
    u"The day ahead does not need to be solved all at once.",
    u"What you care for today can brighten tomorrow.",
    u"You deserve to feel at home in your own life.",
    u"It is never too late for one good moment.",
    u"Your path is allowed to unfold slowly.",
    u"Something lovely can begin without an announcement.",
    u"The calm you create matters.",
    u"You can welcome change without losing yourself.",
    u"A good moment is still good even when it is brief.",
    u"Today is another place where possibility can find you.",
    u"The strength you need may look like taking a pause.",
    u"You have time to become who you are becoming.",
    u"Let hope be practical: take the next kind step.",
    u"Your life has value on the quiet days too.",
    u"There is always room to begin with kindness.",
    u"The effort to care for yourself is meaningful.",
    u"May today give you one reason to smile.",
    u"An interest does not have to become a talent to become a joy.",
    u"Even the things you do with ease began with an awkward first try.",
    u"A thoughtful question can open what a hurried answer leaves closed.",
    u"Knowing what to leave unfinished can make room for what deserves your "
    u"time.",
    u"An ordinary day can become a lasting memory in the right company.",
    u"Keep something in your life whose only measure is the joy it brings.",
    u"Some things speak to you before you have the words to explain why.",
    u"Time shared in laughter stays with us long after the hour has passed.",
    u"Real closeness leaves room for the honest answer.",
    u"A small hello can cross a distance that silence has allowed to grow.",
    u"A few words can carry the warmth of being remembered.",
    u"Curiosity brings us closer than the need to be impressive.",
    u"Sometimes the kindest thing you can offer is a place to be heard.",
    u"A simple thank you can warm the person who made your day.",
    u"A shared laugh can make strangers feel like old friends.",
    u"Some of the moments that shape a life happen between the plans.",
    u"You don't have to wait for an occasion to be thoughtful.",
    u"To remember a small detail is to tell someone they were worth your "
    u"attention.",
    u"Belonging can begin with the simple feeling that someone is glad you "
    u"are here.",
    u"Good company can turn a long task into a short afternoon.",
    u"A passion can make your life richer without becoming your job.",
    u"Create something that would have made your own day brighter.",
    u"An imperfect beginning gives a good idea somewhere to grow.",
    u"An idea begins to teach you when you give it a place in the world.",
    u"A small experiment can teach you something a perfect plan cannot.",
    u"The things that feel foreign today can become part of who you are.",
    u"Leave room in your plans for the person you become along the way.",
    u"Inspiration can light your way without choosing your destination.",
    u"Work made with joy has a worth of its own.",
    u"A small idea brought to life can teach more than a perfect one left in"
    u" your head.",
    u"Some ideas need a quiet season before they are ready to grow.",
    u"Beginning has a way of answering questions that waiting cannot.",
    u"Getting stuck can bring the question you need into focus.",
    u"Being a beginner means there is still so much that can surprise you.",
    u"Skill takes shape in the ordinary hours that nobody thinks to "
    u"celebrate.",
    u"The things you care about deserve room for joy as well as effort.",
    u"An idea can have many beginnings and still become something only you "
    u"would make.",
    u"What you pass by each day may be someone else's place of wonder.",
    u"There are songs you have not heard yet that may one day feel like "
    u"home.",
    u"A familiar place can reveal something new when you bring it your full "
    u"attention.",
    u"A small change in your routine can let a different kind of day unfold.",
    u"Sometimes a walk brings you closer to yourself than to any "
    u"destination.",
    u"A little unplanned time leaves the day room to surprise you.",
    u"A different view can show you what you have stopped noticing.",
    u"A photograph can be a small thank you for something you were glad to "
    u"see.",
    u"The beauty you stop to notice becomes part of your day.",
    u"There are discoveries waiting just beyond the route you know by heart.",
    u"Let the things you love be part of your ordinary days.",
    u"Even a simple meal can become a small celebration when you slow down "
    u"to enjoy it.",
    u"A little sunshine can belong to you even on a day that is not entirely"
    u" bright.",
    u"A familiar song can make the work feel lighter without changing a "
    u"single task.",
    u"Some moments stay with you because you were too busy enjoying them to "
    u"take a picture.",
    u"A quiet room can give your thoughts the space the day has taken from "
    u"them.",
    u"Small comforts become a kind of shelter when you learn to notice them.",
    u"Beauty becomes easier to find when you stop asking it to be "
    u"extraordinary.",
    u"Rest has a value that no list of finished tasks can measure.",
    u"Sometimes you need a little quiet before you can truly listen.",
    u"Good friends can stay close without always being within reach.",
    u"You can wish someone well without following them where you cannot go.",
    u"An honest no can be kinder than a yes you wish you had not given.",
    u"A thoughtful pause can be the beginning of an honest answer.",
    u"The things that feel right to you do not need to win everyone's "
    u"approval.",
    u"You can care about someone without carrying all their worries as your "
    u"own.",
    u"Choose carefully which voices you allow to make a home in your "
    u"thoughts.",
    u"There is a quiet strength in leaving an argument without needing the "
    u"final word.",
    u"Some things need time to settle more than they need another moment of "
    u"worry.",
    u"A full life leaves room for hours that have nothing to prove.",
    u"Make the promises you can keep without losing the person who made "
    u"them.",
    u"There is courage in asking again when you still do not understand.",
    u"What is difficult to grasp today may feel clear when you meet it "
    u"again.",
    u"The courage to ask can carry you further than the comfort of "
    u"pretending to know.",
    u"Discovering you were wrong can be the first thing you get right.",
    u"A mind that can change is still making room to learn.",
    u"Take the wisdom you find in others and give it a place in your own "
    u"life.",
    u"What you do not know can become an invitation rather than a wall.",
    u"Taking time to check is a kindness to the people who trust your "
    u"answer.",
    u"Your words carry more care when they leave room for what you do not "
    u"know.",
    u"A conversation can help you see more, even when it does not change "
    u"your mind.",
    u"A small habit can quietly shape the life that grows around it.",
    u"Give your better intentions a path they can follow on an ordinary day.",
    u"Prepare for tomorrow without giving it every good hour of today.",
    u"What you give your full attention can offer more than what you hurry "
    u"through.",
    u"Make a place for what matters, and it becomes easier to return to it.",
    u"A plan becomes useful when it makes peace with the life you are "
    u"living.",
    u"A small task can carry a larger purpose.",
    u"Begin with something small enough to hold and real enough to build on.",
    u"Leave tomorrow a beginning you will be glad to return to.",
    u"A little care today can become tomorrow's quiet relief.",
    u"Remember what has steadied you when the way ahead feels unfamiliar.",
    u"A good rhythm leaves space for the days when you need to move "
    u"differently.",
    u"One missed day cannot erase the care you have given to all the others.",
    u"A quiet return can be as brave as the first step.",
    u"Growth sometimes begins with the courage to put something down.",
    u"You can tend one part of your life without asking every part to bloom "
    u"at once.",
    u"Lasting change often takes the shape of an ordinary day lived a little"
    u" differently.",
    u"Where you give your attention, you give a little of your life.",
    u"The world will still have questions when you decide to rest.",
    u"Rest can give the world back some of the wonder that tiredness takes "
    u"away.",
    u"Your life continues in the moments when the world cannot reach you.",
    u"Some of what you will treasure most is waiting beyond the edge of a "
    u"screen.",
    u"You can be at peace with a question that does not yet have an answer.",
    u"The joy of playing is a reward you do not have to win.",
    u"A lost match can still belong to an evening you are glad to remember.",
    u"Long after the score is forgotten, the company may still make you "
    u"smile.",
    u"Some jokes are worth keeping for the people they remind you of.",
    u"Listen when your body asks for rest, before it has to raise its voice.",
    u"You can laugh at a mistake without thinking less of yourself.",
    u"Let the unfinished things wait long enough for you to take care of "
    u"yourself.",
    u"A day can be well lived without being won.",
    u"Keep a little foolishness for the days that take themselves too "
    u"seriously.",
    u"Good company can make a simple table feel like a feast.",
    u"The hours that bring you joy do not need to make a good story for "
    u"anyone else.",
    u"A friendship grows comfortable when silence can sit between you "
    u"without feeling like distance.",
    u"Some silences hold the ease of having nothing to prove.",
    u"There is a quiet comfort in being with people you do not have to "
    u"impress.",
    u"You do not have to reach the end of your strength before you let "
    u"someone share the weight.",
    u"A thank you keeps its warmth even when it takes the long way to "
    u"arrive.",
    u"Notice the people who make life kinder in ways that rarely draw "
    u"attention.",
    u"Someone else's good season does not make your own growth less real.",
    u"Another person's happiness can add to your world without taking "
    u"anything from it.",
    u"You can celebrate another person's arrival while still finding your "
    u"own way.",
    u"A sincere compliment gives someone a kinder way to see themselves.",
    u"A welcome spoken warmly can stay with someone long after the door has "
    u"closed.",
    u"An invitation can say you belong, even when you cannot be there.",
    u"Let a good memory keep its own shape while you make room for another.",
    u"What was beautiful does not lose its meaning simply because it came to"
    u" an end.",
    u"You can love a place and still know it is time to leave.",
    u"An old song can hold a new meaning after you have lived a little more.",
    u"With time, you learn what makes you happy and what you only thought "
    u"should.",
    u"What once brought you joy can remain a good chapter without becoming "
    u"the whole story.",
    u"The distance you have traveled can be easier to see when you stop "
    u"measuring every step.",
    u"An old interest can find a new place in the life you have grown into.",
    u"One beautiful moment can remain long after the rough edges of a day "
    u"have faded.",
    u"A small thing to look forward to can give an ordinary day a brighter "
    u"horizon.",
    u"Looking forward to something lets a little of tomorrow's happiness "
    u"reach you today.",
    u"Some of your closest friendships may still be waiting for their first "
    u"hello.",
    u"Joy sometimes arrives by a route you would never have thought to plan.",
    u"A day can turn out kinder than the story you told yourself before it "
    u"began.",
    u"A small surprise can remind you that the day still has something left "
    u"to offer.",
    u"Give a good moment time to reach you before you hurry toward the next "
    u"one.",
    u"A good moment does not have to mend the whole day to deserve a place "
    u"in it.",
    u"A simple pleasure can meet you in the middle of a complicated day.",
    u"One awkward moment is a small page in a story that is still being "
    u"written.",
    u"Your next attempt can begin with more understanding and less blame.",
    u"The day is still unfolding long after your first impression of it.",
    u"A moment of joy is still yours, even after it has passed.",
    u"A life can be quietly beautiful without becoming anyone else's idea of"
    u" extraordinary.",
    u"Let some things be finished so the things you love have room to "
    u"breathe.",
    u"Enough can be a place to rest rather than a line you keep moving.",
    u"Leave a little silence in your life for the things you cannot hear "
    u"while rushing.",
    u"Make time for what restores you, as well as what requires you.",
    u"A quiet kindness does not need to become a story to make a difference.",
    u"Being alone for a while does not make you any less a part of the "
    u"world.",
    u"A small place can hold a great deal of peace when it feels like yours.",
    u"Some of the moments that matter most make sense only to the person "
    u"living them.",
    u"Some things earn their place in your life simply by making it a little"
    u" brighter.",
    u"Let the day be good in a way that belongs to you.",
    u"A little sky above you can make a crowded day feel wider.",
    u"The ordinary days deserve a share of the happiness you keep saving for"
    u" later.",
    u"A chance conversation can leave a thought that keeps you company long "
    u"after the voices fade.",
};

QStringView withoutFinalPeriod(QStringView text)
{
    return text.endsWith(u'.') ? text.chopped(1) : text;
}

template <std::size_t Size>
QString chooseDailyText(const std::array<QStringView, Size> &choices,
                        QRandomGenerator &random, QStringView previous)
{
    static_assert(Size > 1);
    const auto previousChoice = std::find_if(
        choices.begin(), choices.end(), [previous](QStringView choice) {
            return withoutFinalPeriod(choice) == withoutFinalPeriod(previous);
        });
    const bool hasPrevious = previousChoice != choices.end();
    auto index = static_cast<std::size_t>(
        random.bounded(static_cast<quint32>(Size - hasPrevious)));
    if (hasPrevious &&
        index >= static_cast<std::size_t>(previousChoice - choices.begin()))
    {
        ++index;
    }
    return withoutFinalPeriod(choices.at(index)).toString();
}

MessagePtr makeDailyMessage(const QString &quote, const QString &leadIn)
{
    const auto plainText = QStringLiteral("%1 · “%2”").arg(leadIn, quote);

    MessageBuilder builder;
    builder->flags.set(MessageFlag::System);
    builder->flags.set(MessageFlag::Centered);
    builder->flags.set(MessageFlag::DoNotLog);
    builder->flags.set(MessageFlag::DoNotTriggerNotification);
    builder.emplace<TextElement>(
        leadIn,
        MessageElementFlags{MessageElementFlag::Text,
                            MessageElementFlag::AlwaysShow},
        MessageColor::Link, FontStyle::ChatMediumBold);
    builder.emplace<TextElement>(
        QStringLiteral("· “%1”").arg(quote),
        MessageElementFlags{MessageElementFlag::Text,
                            MessageElementFlag::AlwaysShow},
        MessageColor::System);
    builder->messageText = plainText;
    builder->searchText = plainText;
    builder->serverReceivedTime = QDateTime::currentDateTime();
    return builder.release();
}

ChannelPtr selectedDailyMessageChannel()
{
    auto *window = getApp()->getWindows()->getLastSelectedWindow();
    if (window == nullptr)
    {
        return nullptr;
    }
    auto *page = window->getNotebook().getSelectedPage();
    auto *split = page != nullptr ? page->getSelectedSplit() : nullptr;
    auto channel = split != nullptr ? split->getChannel() : nullptr;
    if (!channel || channel->isEmpty())
    {
        return nullptr;
    }

    switch (channel->getType())
    {
        case Channel::Type::Twitch:
        case Channel::Type::Kick:
        case Channel::Type::Multi:
        case Channel::Type::YouTube:
            return channel;
        default:
            return nullptr;
    }
}

}

namespace chatterino {

QString dailyPositiveQuote(QRandomGenerator &random, QStringView previousQuote)
{
    return chooseDailyText(DAILY_QUOTES, random, previousQuote);
}

QString dailyPositiveLeadIn(const QTime &time, QRandomGenerator &random,
                            QStringView previousLeadIn)
{
    const auto *leadIns = &NEW_DAY_LEAD_INS;
    if (time.hour() >= 5 && time.hour() < 12)
    {
        leadIns = &MORNING_LEAD_INS;
    }
    else if (time.hour() >= 12 && time.hour() < 18)
    {
        leadIns = &AFTERNOON_LEAD_INS;
    }
    else if (time.hour() >= 18)
    {
        leadIns = &EVENING_LEAD_INS;
    }

    return chooseDailyText(*leadIns, random, previousLeadIn);
}

bool dailyPositiveMessageIsDue(const QDate &lastShownDate, const QDate &today)
{
    return today.isValid() &&
           (!lastShownDate.isValid() || lastShownDate < today);
}

MoltorinoDailyMessage::MoltorinoDailyMessage(QObject *parent)
    : QObject(parent)
{
    this->timer_.setSingleShot(true);
    QObject::connect(&this->timer_, &QTimer::timeout, this, [this] {
        this->showIfDue();
    });
}

void MoltorinoDailyMessage::start()
{
    if (this->started_)
    {
        return;
    }
    this->started_ = true;
    getSettings()->showDailyPositiveMessage.connect(
        [this](bool enabled) {
            this->timer_.stop();
            if (enabled)
            {
                QTimer::singleShot(0, this, [this] {
                    this->showIfDue();
                });
            }
        },
        this->signalHolder_);
    QTimer::singleShot(1500, this, [this] {
        this->showIfDue();
    });
}

void MoltorinoDailyMessage::showIfDue()
{
    const auto now = QDateTime::currentDateTime();
    const auto today = now.date();
    if (!getSettings()->showDailyPositiveMessage)
    {
        return;
    }

    const auto lastDate = QDate::fromString(
        getSettings()->dailyPositiveMessageLastDate.getValue(), Qt::ISODate);
    if (!dailyPositiveMessageIsDue(lastDate, today))
    {
        this->scheduleNextDay();
        return;
    }

    const auto channel = selectedDailyMessageChannel();
    if (!channel)
    {
        this->scheduleRetry();
        return;
    }

    auto &random = *QRandomGenerator::global();
    const auto quote = dailyPositiveQuote(random, this->lastQuote_.getValue());
    const auto leadIn =
        dailyPositiveLeadIn(now.time(), random, this->lastLeadIn_.getValue());
    channel->addMessage(makeDailyMessage(quote, leadIn),
                        MessageContext::Original);
    this->lastQuote_ = quote;
    this->lastLeadIn_ = leadIn;
    getSettings()->dailyPositiveMessageLastDate = today.toString(Qt::ISODate);
    getSettings()->requestSave();
    this->scheduleNextDay();
}

void MoltorinoDailyMessage::scheduleNextDay()
{
    const auto now = QDateTime::currentDateTime();
    const auto nextDay =
        QDateTime(now.date().addDays(1), QTime(0, 0)).addSecs(1);
    const auto delay = std::clamp<qint64>(
        now.msecsTo(nextDay), 1000,
        static_cast<qint64>(std::numeric_limits<int>::max()));
    this->timer_.start(static_cast<int>(delay));
}

void MoltorinoDailyMessage::scheduleRetry()
{
    this->timer_.start(30'000);
}

}
