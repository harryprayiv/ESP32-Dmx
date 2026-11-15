-- | The keypad command line of the console, as pure functions.
-- |
-- | A command is typed the way the Pockonsole keypad took it:
-- |
-- |     1 @ 50            channel 1 at 50 percent
-- |     1 Thru 10 @ 100   channels 1 to 10 at full
-- |     1 And 5 And 9 @ 0
-- |     1 Thru 10 - 5 @ 75   channels 1 to 10 without channel 5
module Keypad
  ( Digit(..)
  , Key(..)
  , Command
  , digitValue
  , keyLabel
  , showKeys
  , parseCommand
  , percentToLevel
  , slotCount
  ) where

import Prelude

import Data.Array as Array
import Data.Either (Either(..))
import Data.Foldable (foldl)
import Data.Int (round, toNumber)
import Data.List (List(..), (:))
import Data.List as List
import Data.Set (Set)
import Data.Set as Set
import Data.Tuple (Tuple(..))

data Digit = D0 | D1 | D2 | D3 | D4 | D5 | D6 | D7 | D8 | D9

derive instance eqDigit :: Eq Digit

-- | The keys that build up a command. Enter and Delete act on the command
-- | line and are not part of it.
data Key
  = Digit Digit
  | And
  | Minus
  | Thru
  | At

derive instance eqKey :: Eq Key

-- | Slots are 0 based and unique. The level is a DMX value, 0 to 255.
type Command =
  { slots :: Array Int
  , level :: Int
  }

slotCount :: Int
slotCount = 512

digitValue :: Digit -> Int
digitValue = case _ of
  D0 -> 0
  D1 -> 1
  D2 -> 2
  D3 -> 3
  D4 -> 4
  D5 -> 5
  D6 -> 6
  D7 -> 7
  D8 -> 8
  D9 -> 9

-- | The text on the key cap.
keyLabel :: Key -> String
keyLabel = case _ of
  Digit d -> show (digitValue d)
  And -> "&"
  Minus -> "-"
  Thru -> "T"
  At -> "@"

-- | The command line as the operator reads it, for example "1 Thru 10 @ 50".
showKeys :: Array Key -> String
showKeys keys = (foldl step { text: "", lastWasDigit: false } keys).text
  where
  step acc key =
    let
      digit = isDigit key
      gap = if acc.text == "" || (digit && acc.lastWasDigit) then "" else " "
    in
      { text: acc.text <> gap <> word key, lastWasDigit: digit }

  isDigit = case _ of
    Digit _ -> true
    _ -> false

  word = case _ of
    Digit d -> show (digitValue d)
    And -> "And"
    Minus -> "-"
    Thru -> "Thru"
    At -> "@"

-- | A percentage, 0 to 100, as a DMX level.
percentToLevel :: Int -> Int
percentToLevel percent = round (toNumber percent * 255.0 / 100.0)

data Token
  = Number Int
  | Operator Key

-- | Runs of digits become numbers. No channel or level has more than three
-- | digits, so a longer run is refused here.
tokenize :: Array Key -> Either String (List Token)
tokenize keys = finish (foldl step (Right { done: Nil, digits: Nil }) keys)
  where
  step (Left err) _ = Left err
  step (Right acc) key = case key of
    Digit d
      | List.length acc.digits >= 3 -> Left "Number too long"
      | otherwise -> Right acc { digits = digitValue d : acc.digits }
    _ -> Right { done: Operator key : flush acc, digits: Nil }

  finish = map (List.reverse <<< flush)

  flush acc = case acc.digits of
    Nil -> acc.done
    digits -> Number (foldl (\n d -> n * 10 + d) 0 (List.reverse digits)) : acc.done

type Parsed a = Either String (Tuple a (List Token))

channel :: Int -> Either String Int
channel n
  | n >= 1 && n <= slotCount = Right (n - 1)
  | otherwise = Left "Channels are 1 to 512"

range :: List Token -> Parsed (Set Int)
range = case _ of
  Number a : Operator Thru : Number b : rest -> do
    from <- channel a
    to <- channel b
    pure (Tuple (Set.fromFoldable (Array.range (min from to) (max from to))) rest)
  Number _ : Operator Thru : _ -> Left "Thru needs a channel after it"
  Number a : rest -> do
    slot <- channel a
    pure (Tuple (Set.singleton slot) rest)
  _ -> Left "Channel expected"

selection :: List Token -> Parsed (Set Int)
selection tokens = do
  Tuple first rest <- range tokens
  more first rest
  where
  more chosen = case _ of
    Operator And : rest -> do
      Tuple next rest' <- range rest
      more (Set.union chosen next) rest'
    Operator Minus : rest -> do
      Tuple next rest' <- range rest
      more (Set.difference chosen next) rest'
    rest -> pure (Tuple chosen rest)

-- | Parses a finished command line.
parseCommand :: Array Key -> Either String Command
parseCommand keys = do
  tokens <- tokenize keys
  Tuple chosen rest <- selection tokens
  case rest of
    Operator At : Number percent : Nil
      | percent > 100 -> Left "Level is 0 to 100 percent"
      | Set.isEmpty chosen -> Left "No channels selected"
      | otherwise -> Right { slots: Set.toUnfoldable chosen, level: percentToLevel percent }
    Operator At : Nil -> Left "Level missing after @"
    Operator At : _ -> Left "Level is one number, 0 to 100"
    Nil -> Left "@ and a level expected"
    _ -> Left "Expected And, -, Thru or @"
